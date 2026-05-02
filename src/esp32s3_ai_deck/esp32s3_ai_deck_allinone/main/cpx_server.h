#pragma once

/**
 * CPX TCP Server
 *
 * Listens on TCP port 5000 and bridges CPX packets between the host PC
 * and the Crazyflie STM32 via UART (CPX UART transport).
 *
 * Wire format (TCP, matches cflib SocketTransport):
 *   [uint16_t length LE] [byte targetsAndFlags] [byte functionAndVersion] [data...]
 *
 * Wire format (UART, matches cflib UARTTransport):
 *   [0xFF] [uint8_t length] [byte targetsAndFlags] [byte functionAndVersion] [data...] [XOR checksum]
 */

/**
 * Initialize UART for STM32 communication and start the CPX TCP server task.
 * Call once from app_main after WiFi is connected.
 */
void cpx_server_start(void);
