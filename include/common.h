/*
 * common.h - Shared protocol definitions for the BLUE system
 *
 * This file defines the on-the-wire protocol between base station (ESP32-S3)
 * and mobile unit (ESP32 classic). All message types use #pragma pack(1)
 * to ensure deterministic binary layout over ESPNOW.
 *
 * Interview talking points:
 *   - Fixed-size packed structs for zero-copy deserialization
 *   - Magic enums for type safety in a C-struct protocol
 *   - RSSI thresholds chosen empirically: -65 dBm is an "alright/mediocre"
 *     signal; we trigger the extender there (generous), while -55 dBm starts
 *     turning it back off for lasting hysteresis.
 *   - BSSID hashes (32-bit) instead of full MACs to save bandwidth
 *     and avoid leaking AP identity in transit
 */

#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>

#ifndef DEVICE_BASE
#define DEVICE_BASE  0
#endif
#ifndef DEVICE_MOBILE
#define DEVICE_MOBILE 1
#endif

#ifndef DEVICE_TYPE
#define DEVICE_TYPE DEVICE_BASE
#endif

#if __has_include("wifi_creds.h")
#include "wifi_creds.h"
#endif

#define WIFI_CHANNEL         6

#ifndef WIFI_SSID
#define WIFI_SSID            "3bbo"
#endif

#ifndef WIFI_PASS
#define WIFI_PASS            "WeLuvGville202"
#endif

#ifndef REPEATER_SSID
#define REPEATER_SSID        "3bbo_Ext"
#endif

#ifndef REPEATER_PASS
#define REPEATER_PASS        WIFI_PASS
#endif
#define ESPNOW_CHANNEL       6
#define MAX_PEERS            4
#define MAX_SCAN_APS         32
#define TOP_N_RSSI           16
/* Activation thresholds. WEAK is deliberately lenient (-65 dBm, "mediocre"
 * FAIR signal) so the mobile flips the repeater on well before the link
 * collapses; STRONG stays comfortably above it to keep hysteresis and stop
 * the repeater from flapping only a few dBm later. */
#define WEAK_RSSI_THRESHOLD  -65
#define STRONG_RSSI_THRESHOLD -55
#define CONNECTION_TIMEOUT_MS 30000
#define HEARTBEAT_INTERVAL_MS 10000
#define REPORT_INTERVAL_MS    10000
#define STATE_REPORT_INTERVAL_MS 10000
#define DEAD_ZONE_CONFIRM_SCANS   3        /* consecutive empty scans before treating as dead zone */
#define MAP_MAX_ENTRIES       512
#define ML_MAX_SAMPLES        256
#define SCAN_INTERVAL_MS      10000
#define BRIDGE_SAFETY_TIMEOUT_MS 300000
#define REPEATER_MIN_ACTIVE_MS   15000   /* minimum bridge up-time before auto-disarm */
#define REPEATER_RECOVERY_MS     30000   /* sustained strong RSSI before auto-disarm */
#define REPEATER_LED_PIN       2

typedef int8_t rssi_t;
typedef uint8_t battery_pct_t;

enum msg_type_t : uint8_t {
    MSG_FINGERPRINT = 0x01,
    MSG_COMMAND     = 0x02,
    MSG_ACK         = 0x03,
    MSG_HEARTBEAT   = 0x04,
    MSG_MAP_UPDATE  = 0x05,
    MSG_RECONNECT   = 0x06,
    MSG_BUFFERED    = 0x07
};

enum command_t : uint8_t {
    CMD_NONE             = 0x00,
    CMD_ACTIVATE_REPEATER = 0x01,
    CMD_DEACTIVATE       = 0x02,
    CMD_SLEEP            = 0x03,
    CMD_SEND_BUFFERED    = 0x04,
    CMD_SYNC_MAP         = 0x05,
    CMD_SHUTDOWN         = 0x06
};

enum mobile_state_t : uint8_t {
    STATE_SLEEP     = 0x00,
    STATE_SCAN      = 0x01,
    STATE_EVALUATE  = 0x02,
    STATE_REPORT    = 0x03,
    STATE_REPEATER  = 0x04,
    STATE_RECONNECT = 0x05,
    STATE_LOST      = 0x06
};

enum trigger_reason_t : uint8_t {
    REASON_NONE       = 0x00,
    REASON_LOCAL_RSSI = 0x01,
    REASON_PREDICTIVE = 0x02,
    RE_REASON_MANUAL  = 0x03
};

/* Who asked for the mobile's repeater to be on. Carried in both command
 * (base -> mobile) and fingerprint/status (mobile -> base) messages so the
 * dashboard can show WHY the extender is running. */
enum activation_source_t : uint8_t {
    SRC_NONE        = 0x00,   /* repeater off / no trigger */
    SRC_MOBILE_RSSI = 0x01,   /* mobile detected weak signal itself */
    SRC_BASE_ML     = 0x02,   /* base ML prediction (known-bad zone/time) */
    SRC_BASE_MANUAL = 0x03    /* operator push */
};

enum deactivation_reason_t : uint8_t {
    DEACT_BASE_CMD       = 0x00,
    DEACT_LOCAL_RECOVERY = 0x01,
    DEACT_SAFETY_TIMEOUT = 0x02,
    DEACT_LOW_BATTERY    = 0x03
};

enum connection_status_t : uint8_t {
    CONNECTED       = 0x00,
    DEGRADED        = 0x01,
    LOST            = 0x02,
    RECONNECTING    = 0x03
};

enum wifi_quality_t : uint8_t {
    WIFI_QUAL_EXCELLENT = 0,
    WIFI_QUAL_GOOD      = 1,
    WIFI_QUAL_FAIR      = 2,
    WIFI_QUAL_POOR      = 3,
    WIFI_QUAL_DEAD      = 4
};

enum loss_reason_t : uint8_t {
    LOSS_TIMEOUT       = 0x00,
    LOSS_RADIO_ERROR   = 0x01,
    LOSS_DEVICE_SLEEP  = 0x02,
    LOSS_OUT_OF_RANGE  = 0x03
};

#pragma pack(push, 1)

struct fingerprint_msg_t {
    uint8_t  msg_type;
    uint32_t timestamp;
    rssi_t   rssi_values[TOP_N_RSSI];
    uint32_t bssid_hashes[TOP_N_RSSI];
    uint8_t  scan_count;
    battery_pct_t battery_pct;
    mobile_state_t local_state;
    rssi_t   local_rssi_avg;
    uint8_t  sequence_num;
    activation_source_t activation_source;
    uint8_t  confidence_pct;
};

struct command_msg_t {
    uint8_t  msg_type;
    uint8_t  command;
    uint8_t  bridge_channel;
    uint32_t bridge_timeout_ms;
    uint32_t sequence_num;
    uint32_t expected_ack;
    activation_source_t source;
    uint8_t  confidence_pct;
};

struct ack_msg_t {
    uint8_t  msg_type;
    uint8_t  ack_for_type;
    uint32_t ack_for_seq;
    uint8_t  status;
};

struct heartbeat_msg_t {
    uint8_t  msg_type;
    uint32_t timestamp;
    uint32_t sequence_num;
    uint8_t  connection_quality;
    uint8_t  active_clients;
};

struct reconnect_msg_t {
    uint8_t  msg_type;
    uint8_t  reason;
    uint32_t last_seen_timestamp;
    uint8_t  buffered_count;
};

struct rf_map_entry_t {
    uint32_t bssid_hash;
    uint8_t  hour_of_day;
    uint8_t  day_of_week;
    rssi_t   rssi_mean;
    rssi_t   rssi_std;
    uint16_t sample_count;
    bool     is_weak;
};

struct weak_zone_prediction_t {
    uint32_t bssid_hash;
    uint8_t  hour_of_day;
    float    predicted_rssi;
    float    confidence;
    bool     activation_recommended;
};

#pragma pack(pop)

#endif
