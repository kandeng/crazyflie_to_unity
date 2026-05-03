/*
 * cpx_server.c
 *
 * CPX (Crazyflie Packet eXchange) TCP↔UART bridge for ESP32-S3 AI Deck.
 *
 * UART protocol (matches official aideck-esp-firmware uart_transport.c):
 *   Frame:   [0xFF][payloadLen][payload...][XOR-CRC]
 *   Sync:    [0xFF][0x00]  (CTS/CTR flow-control token)
 *
 *   Flow control:
 *     - TX side sends [0xFF 0x00] tokens repeatedly until it receives one back (CTS).
 *     - After receiving a data packet, the receiver immediately sends [0xFF 0x00] (CTR)
 *       to allow the next packet.
 *     - Both sides must exchange a CTS before any data packet can be sent.
 *
 * TCP framing (matches cflib SocketTransport):
 *   [uint16_t payloadLen LE][payload...]
 *
 * UART pins: GPIO43 (TXD0) and GPIO44 (RXD0) — connected to Crazyflie expansion P2.
 * These are the physical UART pins to the STM32; we use UART_NUM_1 mapped to them
 * while the console runs through the USB-JTAG peripheral (not UART0).
 */

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "driver/uart.h"

#include "cpx_server.h"

static const char *TAG = "CPX";

/* -----------------------------------------------------------------------
 * Hardware / protocol configuration
 * ----------------------------------------------------------------------- */
#define CPX_UART_PORT    UART_NUM_1
#define CPX_UART_TX_PIN  43          /* TXD0 → Crazyflie RX (P2 pin 2) */
#define CPX_UART_RX_PIN  44          /* RXD0 → Crazyflie TX (P2 pin 1) */
#define CPX_UART_BAUD    576000
#define CPX_UART_BUF_SZ  1024

#define CPX_TCP_PORT     5000
#define CPX_MAX_PKT_SIZE 256         /* max CPX payload bytes (uint8_t len ≤ 255) */

/* Event bits for UART flow-control */
#define CTS_EVENT  (1 << 0)   /* "Clear To Send" — remote is ready to receive */
#define CTR_EVENT  (1 << 1)   /* "Clear To Receive" — we need to ack a packet  */
#define TXQ_EVENT  (1 << 2)   /* data waiting in tx queue */

/* -----------------------------------------------------------------------
 * Shared state
 * ----------------------------------------------------------------------- */
static int               g_client_sock = -1;
static SemaphoreHandle_t g_sock_mutex;

static EventGroupHandle_t g_uart_evg;   /* CTS/CTR/TXQ flow control */

/* Simple single-slot TX queue: tcp_server_task stores a packet,
 * uart_tx_task drains it. Protected by g_uart_tx_mutex. */
static SemaphoreHandle_t  g_uart_tx_mutex;
static uint8_t            g_uart_tx_buf[CPX_MAX_PKT_SIZE];
static uint8_t            g_uart_tx_len;
static bool               g_uart_tx_pending;

/* -----------------------------------------------------------------------
 * UART low-level helpers
 * ----------------------------------------------------------------------- */
static uint8_t calc_crc(const uint8_t *buf, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) crc ^= buf[i];
    return crc;
}

/* Send the two-byte sync/flow-control token [0xFF 0x00] */
static void uart_send_token(void)
{
    uint8_t tok[2] = {0xFF, 0x00};
    uart_write_bytes(CPX_UART_PORT, tok, 2);
}

/* Send a full CPX packet over UART */
static void uart_send_packet(const uint8_t *payload, uint8_t len)
{
    /* buf = [0xFF][len][payload...][crc] */
    uint8_t buf[2 + CPX_MAX_PKT_SIZE + 1];
    buf[0] = 0xFF;
    buf[1] = len;
    memcpy(&buf[2], payload, len);
    buf[2 + len] = calc_crc(buf, 2 + len);
    uart_write_bytes(CPX_UART_PORT, buf, 2 + len + 1);
}

/* -----------------------------------------------------------------------
 * Task: UART TX  (sends queued packets to STM32 with flow-control)
 *
 * Mirrors the official uart_tx_task logic:
 *   1. On startup, send [0xFF 0x00] tokens until CTS received.
 *   2. When a packet is queued (TXQ_EVENT), wait for CTS, then send packet.
 *   3. When we receive CTR_EVENT, send a [0xFF 0x00] back (acknowledge).
 * ----------------------------------------------------------------------- */
static void uart_tx_task(void *arg)
{
    EventBits_t evBits;

    /* Step 1: Handshake — send tokens until remote says CTS */
    ESP_LOGI(TAG, "UART TX: starting handshake...");
    do {
        uart_send_token();
        vTaskDelay(pdMS_TO_TICKS(10));
        evBits = xEventGroupGetBits(g_uart_evg);
    } while (!(evBits & CTS_EVENT));
    ESP_LOGI(TAG, "UART TX: handshake complete");

    while (true) {
        /* Wait for something to do */
        if (g_uart_tx_pending == false) {
            ESP_LOGD(TAG, "UART TX: waiting for CTR/TXQ");
            evBits = xEventGroupWaitBits(g_uart_evg,
                                         CTR_EVENT | TXQ_EVENT,
                                         pdTRUE,   /* clear on return */
                                         pdFALSE,  /* any bit */
                                         portMAX_DELAY);
            if (evBits & CTR_EVENT) {
                uart_send_token();
                ESP_LOGD(TAG, "UART TX: sent CTR");
            }
        }

        if (g_uart_tx_pending) {
            /* Wait for CTS before sending */
            do {
                ESP_LOGD(TAG, "UART TX: waiting for CTR/CTS");
                evBits = xEventGroupWaitBits(g_uart_evg,
                                             CTR_EVENT | CTS_EVENT,
                                             pdTRUE,
                                             pdFALSE,
                                             portMAX_DELAY);
                if (evBits & CTR_EVENT) {
                    uart_send_token();
                    ESP_LOGD(TAG, "UART TX: sent CTR while waiting for CTS");
                }
            } while (!(evBits & CTS_EVENT));

            /* Grab and send the packet */
            xSemaphoreTake(g_uart_tx_mutex, portMAX_DELAY);
            uint8_t len = g_uart_tx_len;
            uint8_t tmp[CPX_MAX_PKT_SIZE];
            memcpy(tmp, g_uart_tx_buf, len);
            g_uart_tx_pending = false;
            xSemaphoreGive(g_uart_tx_mutex);

            uart_send_packet(tmp, len);
            ESP_LOGD(TAG, "UART TX: sent packet len=%d", len);
        }
    }
}

/* -----------------------------------------------------------------------
 * Task: UART RX  (receives packets from STM32, forwards to TCP client)
 *
 * Mirrors the official uart_rx_task logic:
 *   - [0xFF][0x00] → set CTS_EVENT
 *   - [0xFF][len][payload...][crc] → verify CRC, forward to TCP, send CTR
 * ----------------------------------------------------------------------- */
static void uart_rx_task(void *arg)
{
    uint8_t payload[CPX_MAX_PKT_SIZE];

    while (true) {
        /* Wait for 0xFF start byte */
        uint8_t start;
        do {
            uart_read_bytes(CPX_UART_PORT, &start, 1, portMAX_DELAY);
        } while (start != 0xFF);

        /* Read length */
        uint8_t len;
        uart_read_bytes(CPX_UART_PORT, &len, 1, portMAX_DELAY);

        if (len == 0) {
            /* CTS token from STM32 */
            ESP_LOGD(TAG, "UART RX: got CTS");
            xEventGroupSetBits(g_uart_evg, CTS_EVENT);
            continue;
        }

        /* Read payload + CRC */
        uart_read_bytes(CPX_UART_PORT, payload, len, portMAX_DELAY);
        uint8_t rx_crc;
        uart_read_bytes(CPX_UART_PORT, &rx_crc, 1, portMAX_DELAY);

        /* Verify CRC */
        uint8_t hdr[2] = {0xFF, len};
        uint8_t expected = calc_crc(hdr, 2);
        expected ^= calc_crc(payload, len);
        if (expected != rx_crc) {
            ESP_LOGW(TAG, "UART RX: CRC error (got 0x%02x expected 0x%02x)", rx_crc, expected);
            /* Still send CTR so link doesn't stall */
            xEventGroupSetBits(g_uart_evg, CTR_EVENT);
            continue;
        }

        ESP_LOGD(TAG, "UART RX: received packet len=%d", len);

        /* Forward to TCP client */
        xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
        int sock = g_client_sock;
        xSemaphoreGive(g_sock_mutex);

        if (sock >= 0) {
            /* TCP frame: [uint16_t len LE][payload] */
            uint8_t hdr_tcp[2] = {(uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
            send(sock, hdr_tcp, 2, 0);
            send(sock, payload, len, 0);
        }

        /* Acknowledge receipt — send CTR to allow next packet */
        xEventGroupSetBits(g_uart_evg, CTR_EVENT);
    }
}

/* -----------------------------------------------------------------------
 * Task: TCP server  (accepts one client, reads TCP → UART TX queue)
 * ----------------------------------------------------------------------- */
static void tcp_server_task(void *arg)
{
    int opt = 1;
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(CPX_TCP_PORT),
    };

    int server_sock = -1;

    while (true) {
        /* (Re)create server socket */
        server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (server_sock < 0) {
            ESP_LOGE(TAG, "socket() failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        if (bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
            listen(server_sock, 1) != 0) {
            ESP_LOGE(TAG, "bind/listen failed: errno %d", errno);
            close(server_sock);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGI(TAG, "CPX TCP server listening on port %d", CPX_TCP_PORT);

        while (true) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
            if (client_sock < 0) {
                ESP_LOGW(TAG, "accept failed: errno %d — recreating server socket", errno);
                close(server_sock);
                break; /* break inner loop → recreate server socket */
            }
            ESP_LOGI(TAG, "Client connected");

            xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
            g_client_sock = client_sock;
            xSemaphoreGive(g_sock_mutex);

            /* Read TCP frames and queue them for UART TX */
            uint8_t payload[CPX_MAX_PKT_SIZE];
            while (true) {
                uint8_t hdr[2];
                int r = recv(client_sock, hdr, 2, MSG_WAITALL);
                if (r != 2) break;

                uint16_t plen = (uint16_t)(hdr[0] | ((uint16_t)hdr[1] << 8));
                if (plen == 0 || plen > CPX_MAX_PKT_SIZE) {
                    ESP_LOGW(TAG, "Bad TCP packet length %d", plen);
                    break;
                }
                r = recv(client_sock, payload, plen, MSG_WAITALL);
                if (r != plen) break;

                /* Queue for UART TX — wait if previous not yet consumed */
                xSemaphoreTake(g_uart_tx_mutex, portMAX_DELAY);
                while (g_uart_tx_pending) {
                    xSemaphoreGive(g_uart_tx_mutex);
                    vTaskDelay(pdMS_TO_TICKS(1));
                    xSemaphoreTake(g_uart_tx_mutex, portMAX_DELAY);
                }
                memcpy(g_uart_tx_buf, payload, plen);
                g_uart_tx_len = (uint8_t)plen;
                g_uart_tx_pending = true;
                xSemaphoreGive(g_uart_tx_mutex);

                xEventGroupSetBits(g_uart_evg, TXQ_EVENT);
            }

            ESP_LOGI(TAG, "Client disconnected");
            xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
            if (g_client_sock == client_sock) {
                close(g_client_sock);
                g_client_sock = -1;
            }
            xSemaphoreGive(g_sock_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
void cpx_server_start(void)
{
    /* Configure UART1 on GPIO43 (TX) / GPIO44 (RX) */
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

    g_sock_mutex     = xSemaphoreCreateMutex();
    g_uart_tx_mutex  = xSemaphoreCreateMutex();
    g_uart_evg       = xEventGroupCreate();
    configASSERT(g_sock_mutex && g_uart_tx_mutex && g_uart_evg);

    g_uart_tx_pending = false;

    /* Start tasks:
     *   uart_rx_task  — always-on UART receiver, feeds CTS events and TCP
     *   uart_tx_task  — handshakes then sends queued packets to STM32
     *   tcp_server_task — accepts TCP connection, queues packets for UART TX
     */
    xTaskCreate(uart_rx_task,    "cpx_urx", 4096, NULL, 7, NULL);
    xTaskCreate(uart_tx_task,    "cpx_utx", 4096, NULL, 6, NULL);
    xTaskCreate(tcp_server_task, "cpx_tcp", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "CPX server started (UART%d TX=%d RX=%d @ %d baud, TCP port %d)",
             CPX_UART_PORT, CPX_UART_TX_PIN, CPX_UART_RX_PIN,
             CPX_UART_BAUD, CPX_TCP_PORT);
}
