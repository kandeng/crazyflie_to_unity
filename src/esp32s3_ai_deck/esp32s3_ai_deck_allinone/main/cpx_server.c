#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/uart.h"

#include "cpx_server.h"

static const char *TAG = "CPX";

/* -----------------------------------------------------------------------
 * Hardware configuration
 * ESP32-S3 AI-Deck UART pins toward STM32 (Crazyflie main MCU).
 * TX = GPIO 43, RX = GPIO 44  (standard ESP32-S3 UART0 / AI-deck pinout).
 * Adjust if your board uses different pins.
 * ----------------------------------------------------------------------- */
#define CPX_UART_PORT    UART_NUM_1
#define CPX_UART_TX_PIN  43
#define CPX_UART_RX_PIN  44
#define CPX_UART_BAUD    576000
#define CPX_UART_BUF_SZ  1024

/* TCP server */
#define CPX_TCP_PORT     5000
#define CPX_MAX_PKT_SIZE 256   /* max CPX payload bytes */

/* -----------------------------------------------------------------------
 * Shared state
 * ----------------------------------------------------------------------- */
static int           g_client_sock = -1;
static SemaphoreHandle_t g_sock_mutex;   /* protects g_client_sock writes */

/* -----------------------------------------------------------------------
 * UART helpers (CPX UART framing, matches cflib UARTTransport)
 *
 * Frame: 0xFF | len(1 byte) | payload(len bytes) | XOR-checksum(1 byte)
 * where payload = [targetsAndFlags, functionAndVersion, data...]
 * CRC covers the 0xFF, len, and payload bytes.
 * ----------------------------------------------------------------------- */
static uint8_t uart_xor_checksum(const uint8_t *buf, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
    }
    return crc;
}

/**
 * Send one CPX packet payload (targetsAndFlags + functionAndVersion + data)
 * over UART with proper framing.
 * Returns true on success.
 */
static bool uart_send_cpx(const uint8_t *payload, uint8_t payload_len)
{
    if (payload_len == 0) {
        return false;
    }
    /* Build framed buffer: [0xFF][len][payload...][crc] */
    uint8_t buf[2 + CPX_MAX_PKT_SIZE + 1];
    buf[0] = 0xFF;
    buf[1] = payload_len;
    memcpy(&buf[2], payload, payload_len);
    buf[2 + payload_len] = uart_xor_checksum(buf, 2 + payload_len);

    int written = uart_write_bytes(CPX_UART_PORT, buf, 2 + payload_len + 1);
    return (written == (int)(2 + payload_len + 1));
}

/**
 * Receive one CPX packet from UART (blocking).
 * Fills payload_out (caller must supply CPX_MAX_PKT_SIZE bytes).
 * Returns payload length on success, -1 on framing/CRC error.
 */
static int uart_recv_cpx(uint8_t *payload_out)
{
    uint8_t byte;

    /* Wait for 0xFF start byte */
    while (true) {
        int r = uart_read_bytes(CPX_UART_PORT, &byte, 1, portMAX_DELAY);
        if (r < 0) return -1;
        if (byte == 0xFF) break;
    }

    /* Read length */
    uint8_t len = 0;
    if (uart_read_bytes(CPX_UART_PORT, &len, 1, pdMS_TO_TICKS(100)) != 1) return -1;
    if (len == 0) {
        /* CTS / sync pulse — ignore */
        return 0;
    }
    /* uint8_t max (255) fits within CPX_MAX_PKT_SIZE (256) by design */

    /* Read payload */
    if (uart_read_bytes(CPX_UART_PORT, payload_out, len, pdMS_TO_TICKS(200)) != len) return -1;

    /* Read and verify XOR checksum */
    uint8_t rx_crc;
    if (uart_read_bytes(CPX_UART_PORT, &rx_crc, 1, pdMS_TO_TICKS(100)) != 1) return -1;

    uint8_t header[2] = {0xFF, len};
    uint8_t calc_crc = uart_xor_checksum(header, 2);
    calc_crc ^= uart_xor_checksum(payload_out, len);

    if (calc_crc != rx_crc) {
        ESP_LOGW(TAG, "UART CRC error (got 0x%02x, expected 0x%02x)", rx_crc, calc_crc);
        return -1;
    }

    return (int)len;
}

/* -----------------------------------------------------------------------
 * TCP helpers (CPX socket framing, matches cflib SocketTransport)
 *
 * Frame: uint16_t length (LE) | payload(length bytes)
 * where payload = [targetsAndFlags, functionAndVersion, data...]
 * Note: the length field = payload_len + 2 (includes the 2 header bytes
 *       that are part of wireData in cflib but NOT the uint16 itself).
 *       Actually in cflib writePacket:
 *         data = pack('H', packet.length+2)  -> length of wireData
 *         data += packet.wireData             -> wireData = [t&f, f&v, ...]
 *       And readPacket:
 *         size = unpack('H', read(2))[0]
 *         data = read(size)                   -> wireData
 *       So: the uint16 is exactly len(wireData) = payload_len.
 * ----------------------------------------------------------------------- */
static bool tcp_send_all(int sock, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int r = send(sock, buf + sent, len - sent, 0);
        if (r <= 0) return false;
        sent += r;
    }
    return true;
}

static bool tcp_recv_all(int sock, uint8_t *buf, size_t len)
{
    size_t received = 0;
    while (received < len) {
        int r = recv(sock, buf + received, len - received, 0);
        if (r <= 0) return false;
        received += r;
    }
    return true;
}

/**
 * Send CPX payload over TCP with the SocketTransport framing.
 */
static bool tcp_send_cpx(int sock, const uint8_t *payload, uint16_t payload_len)
{
    /* length field = payload_len (wireData length) */
    uint8_t hdr[2];
    hdr[0] = (uint8_t)(payload_len & 0xFF);
    hdr[1] = (uint8_t)((payload_len >> 8) & 0xFF);

    if (!tcp_send_all(sock, hdr, 2)) return false;
    if (!tcp_send_all(sock, payload, payload_len)) return false;
    return true;
}

/**
 * Read one CPX frame from TCP.
 * Returns payload length, 0 if connection closed, -1 on error.
 */
static int tcp_recv_cpx(int sock, uint8_t *payload_out)
{
    uint8_t hdr[2];
    if (!tcp_recv_all(sock, hdr, 2)) return 0;

    uint16_t payload_len = (uint16_t)(hdr[0] | ((uint16_t)hdr[1] << 8));
    if (payload_len == 0 || payload_len > CPX_MAX_PKT_SIZE) return -1;

    if (!tcp_recv_all(sock, payload_out, payload_len)) return 0;
    return (int)payload_len;
}

/* -----------------------------------------------------------------------
 * Task: UART → TCP  (STM32 to host)
 * ----------------------------------------------------------------------- */
static void uart_to_tcp_task(void *arg)
{
    uint8_t payload[CPX_MAX_PKT_SIZE];

    while (true) {
        int len = uart_recv_cpx(payload);
        if (len <= 0) {
            if (len < 0) {
                ESP_LOGW(TAG, "UART recv error, skipping packet");
            }
            continue;
        }

        xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
        int sock = g_client_sock;
        xSemaphoreGive(g_sock_mutex);

        if (sock < 0) continue;  /* no client connected */

        if (!tcp_send_cpx(sock, payload, (uint16_t)len)) {
            ESP_LOGW(TAG, "TCP send failed, client disconnected?");
            xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
            if (g_client_sock == sock) {
                close(g_client_sock);
                g_client_sock = -1;
            }
            xSemaphoreGive(g_sock_mutex);
        }
    }
}

/* -----------------------------------------------------------------------
 * Task: TCP server — accepts one client at a time, reads TCP → UART
 * ----------------------------------------------------------------------- */
static void tcp_server_task(void *arg)
{
    int server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_sock < 0) {
        ESP_LOGE(TAG, "Failed to create server socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(CPX_TCP_PORT),
    };

    if (bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind failed: errno %d", errno);
        close(server_sock);
        vTaskDelete(NULL);
        return;
    }

    if (listen(server_sock, 1) != 0) {
        ESP_LOGE(TAG, "listen failed: errno %d", errno);
        close(server_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "CPX TCP server listening on port %d", CPX_TCP_PORT);

    uint8_t payload[CPX_MAX_PKT_SIZE];

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock < 0) {
            ESP_LOGW(TAG, "accept failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        ESP_LOGI(TAG, "Client connected");

        xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
        g_client_sock = client_sock;
        xSemaphoreGive(g_sock_mutex);

        /* Read TCP packets and forward to STM32 via UART */
        while (true) {
            int len = tcp_recv_cpx(client_sock, payload);
            if (len == 0) {
                ESP_LOGI(TAG, "Client disconnected");
                break;
            }
            if (len < 0) {
                ESP_LOGW(TAG, "TCP recv error, dropping packet");
                continue;
            }
            if (!uart_send_cpx(payload, (uint8_t)len)) {
                ESP_LOGW(TAG, "UART send failed");
            }
        }

        xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
        close(g_client_sock);
        g_client_sock = -1;
        xSemaphoreGive(g_sock_mutex);
    }
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
void cpx_server_start(void)
{
    /* Configure UART */
    uart_config_t uart_cfg = {
        .baud_rate  = CPX_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(CPX_UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(CPX_UART_PORT,
                                 CPX_UART_TX_PIN, CPX_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(CPX_UART_PORT,
                                        CPX_UART_BUF_SZ, CPX_UART_BUF_SZ,
                                        0, NULL, 0));

    g_sock_mutex = xSemaphoreCreateMutex();
    configASSERT(g_sock_mutex);

    /* Start UART→TCP forwarder (higher priority, always runs) */
    xTaskCreate(uart_to_tcp_task, "cpx_u2t", 4096, NULL, 6, NULL);

    /* Start TCP server (accepts connections, reads TCP→UART) */
    xTaskCreate(tcp_server_task, "cpx_tcp", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "CPX server started (UART%d TX=%d RX=%d @ %d baud, TCP port %d)",
             CPX_UART_PORT, CPX_UART_TX_PIN, CPX_UART_RX_PIN,
             CPX_UART_BAUD, CPX_TCP_PORT);
}
