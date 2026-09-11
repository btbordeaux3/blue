/*
 * espnow_comm.cpp - ESPNOW transport layer for BLUE
 *
 * Provides reliable message delivery over ESPNOW (Espressif's low-level
 * peer-to-peer protocol). ESPNOW operates on a single WiFi channel and
 * requires both peers to be on the same channel to communicate.
 *
 * Architecture decisions:
 *   - Callback-based receive dispatch (not polling) for lowest latency
 *   - Channel sync via WiFi.channel() (Arduino API) instead of
 *     esp_wifi_get_channel() (ESP-IDF API) because the latter crashes
 *     on ESP32 classic during light sleep wake cycle
 *   - Peer management: always delete-then-add to avoid stale channel refs
 *   - metrics_track_tx/rx for real-time system health monitoring
 *
 * Interview talking points:
 *   - ESPNOW is L2 (data link layer) - no IP overhead, ~3ms latency
 *   - Channel must match between peers; our system syncs via WiFi channel
 *   - Send callback fires asynchronously; we count failures for reliability
 *   - Broadcast address (FF:FF:FF:FF:FF:FF) for initial discovery
 */

#include "espnow_comm.h"
#include "metrics.h"

static uint8_t peer_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static uint8_t device_type = DEVICE_BASE;
static espnow_cmd_callback_t cmd_cb = nullptr;
static espnow_report_callback_t report_cb = nullptr;
static espnow_heartbeat_callback_t hb_cb = nullptr;
static espnow_reconnect_callback_t re_cb = nullptr;
static connection_status_t conn_status = LOST;
static uint32_t last_received_ms = 0;
static uint32_t tx_count = 0;
static uint32_t tx_fail_count = 0;

static void on_data_sent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS) {
        tx_fail_count++;
        metrics_track_tx_fail();
    } else {
        metrics_track_tx();
    }
}

static void on_data_recv(const esp_now_recv_info *info, const uint8_t *data, int len) {
    const uint8_t *mac = info->src_addr;
    if (len < 1) return;

    last_received_ms = millis();
    conn_status = CONNECTED;
    metrics_track_rx();

    uint8_t msg_type = data[0];

    switch (msg_type) {
        case MSG_FINGERPRINT: {
            if (len >= sizeof(fingerprint_msg_t) && report_cb) {
                fingerprint_msg_t msg;
                memcpy(&msg, data, sizeof(fingerprint_msg_t));
                report_cb(&msg, mac);
            }
            break;
        }
        case MSG_COMMAND: {
            if (len >= sizeof(command_msg_t) && cmd_cb) {
                command_msg_t msg;
                memcpy(&msg, data, sizeof(command_msg_t));
                cmd_cb(&msg);
            }
            break;
        }
        case MSG_HEARTBEAT: {
            if (len >= sizeof(heartbeat_msg_t) && hb_cb) {
                heartbeat_msg_t msg;
                memcpy(&msg, data, sizeof(heartbeat_msg_t));
                hb_cb(&msg, mac);
            }
            break;
        }
        case MSG_RECONNECT: {
            if (len >= sizeof(reconnect_msg_t) && re_cb) {
                reconnect_msg_t msg;
                memcpy(&msg, data, sizeof(reconnect_msg_t));
                re_cb(&msg, mac);
            }
            break;
        }
        case MSG_ACK: {
            break;
        }
        default:
            break;
    }
}

bool espnow_comm_init(uint8_t type, const uint8_t *peer) {
    device_type = type;
    memcpy(peer_mac, peer, 6);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESPNOW] init failed");
        return false;
    }

    esp_now_register_send_cb(on_data_sent);
    esp_now_register_recv_cb(on_data_recv);

    espnow_add_peer(peer_mac);

    Serial.printf("[ESPNOW] initialized as %s, peer: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  type == DEVICE_BASE ? "BASE" : "MOBILE",
                  peer[0], peer[1], peer[2], peer[3], peer[4], peer[5]);

    return true;
}

void espnow_add_peer(const uint8_t *mac) {
    if (esp_now_is_peer_exist(mac)) {
        esp_now_del_peer(mac);
    }
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
}

void espnow_remove_peer(const uint8_t *mac) {
    esp_now_del_peer(mac);
}

uint8_t espnow_sync_channel(void) {
    uint8_t ch = WiFi.channel();
    if (ch != 0) {
        if (esp_now_is_peer_exist(peer_mac)) {
            esp_now_del_peer(peer_mac);
        }
        esp_now_peer_info_t peer;
        memset(&peer, 0, sizeof(peer));
        memcpy(peer.peer_addr, peer_mac, 6);
        peer.channel = ch;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
        static uint8_t last_ch = 0;
        if (ch != last_ch) {
            Serial.printf("[ESPNOW] Synced peer to channel %d\n", ch);
            last_ch = ch;
        }
    }
    return ch;
}

static bool espnow_send_raw(const uint8_t *data, int len) {
    esp_err_t result = esp_now_send(peer_mac, data, len);
    tx_count++;
    return (result == ESP_OK);
}

bool espnow_send_fingerprint(const fingerprint_msg_t *msg) {
    return espnow_send_raw((const uint8_t *)msg, sizeof(fingerprint_msg_t));
}

bool espnow_send_command(const command_msg_t *msg) {
    return espnow_send_raw((const uint8_t *)msg, sizeof(command_msg_t));
}

bool espnow_send_ack(uint8_t for_type, uint32_t for_seq, uint8_t status) {
    ack_msg_t ack;
    ack.msg_type = MSG_ACK;
    ack.ack_for_type = for_type;
    ack.ack_for_seq = for_seq;
    ack.status = status;
    return espnow_send_raw((const uint8_t *)&ack, sizeof(ack_msg_t));
}

bool espnow_send_heartbeat(const heartbeat_msg_t *msg) {
    return espnow_send_raw((const uint8_t *)msg, sizeof(heartbeat_msg_t));
}

bool espnow_send_reconnect(const reconnect_msg_t *msg) {
    return espnow_send_raw((const uint8_t *)msg, sizeof(reconnect_msg_t));
}

void espnow_set_cmd_callback(espnow_cmd_callback_t cb) { cmd_cb = cb; }
void espnow_set_report_callback(espnow_report_callback_t cb) { report_cb = cb; }
void espnow_set_heartbeat_callback(espnow_heartbeat_callback_t cb) { hb_cb = cb; }
void espnow_set_reconnect_callback(espnow_reconnect_callback_t cb) { re_cb = cb; }

connection_status_t espnow_get_connection_status(void) {
    if (conn_status == CONNECTED && (millis() - last_received_ms > CONNECTION_TIMEOUT_MS)) {
        conn_status = LOST;
    }
    return conn_status;
}

uint32_t espnow_get_last_received_ms(void) {
    return last_received_ms;
}

void espnow_print_peers(void) {
    Serial.printf("[ESPNOW] TX: %lu, TX fail: %lu, last RX: %lu ms ago, status: %d\n",
                  tx_count, tx_fail_count,
                  millis() - last_received_ms,
                  conn_status);
}
