/*
 * espnow_comm.h - ESPNOW communication abstraction layer
 *
 * Provides a clean API over ESP-IDF's ESPNOW with:
 *   - Typed message dispatch (callback per message type)
 *   - Peer management with channel synchronization
 *   - Connection status tracking with timeout detection
 *   - Typed send functions for each message type
 *
 * Interview talking points:
 *   - Callback pattern decouples transport from application logic
 *   - espnow_sync_channel() resolves the fundamental ESPNOW limitation:
 *     peers must share the same WiFi channel
 *   - Connection status is derived, not stored (timeout-based)
 */

#ifndef ESPNOW_COMM_H
#define ESPNOW_COMM_H

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include "common.h"

typedef void (*espnow_cmd_callback_t)(const command_msg_t *cmd);
typedef void (*espnow_report_callback_t)(const fingerprint_msg_t *report, const uint8_t *mac);
typedef void (*espnow_heartbeat_callback_t)(const heartbeat_msg_t *hb, const uint8_t *mac);
typedef void (*espnow_reconnect_callback_t)(const reconnect_msg_t *re, const uint8_t *mac);

bool espnow_comm_init(uint8_t device_type, const uint8_t *peer_mac);
void espnow_add_peer(const uint8_t *mac);
void espnow_remove_peer(const uint8_t *mac);
uint8_t espnow_sync_channel(void);
bool espnow_send_fingerprint(const fingerprint_msg_t *msg);
bool espnow_send_command(const command_msg_t *msg);
bool espnow_send_ack(uint8_t for_type, uint32_t for_seq, uint8_t status);
bool espnow_send_heartbeat(const heartbeat_msg_t *msg);
bool espnow_send_reconnect(const reconnect_msg_t *msg);
void espnow_set_cmd_callback(espnow_cmd_callback_t cb);
void espnow_set_report_callback(espnow_report_callback_t cb);
void espnow_set_heartbeat_callback(espnow_heartbeat_callback_t cb);
void espnow_set_reconnect_callback(espnow_reconnect_callback_t cb);
connection_status_t espnow_get_connection_status(void);
uint32_t espnow_get_last_received_ms(void);
void espnow_print_peers(void);

#endif
