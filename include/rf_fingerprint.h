/*
 * rf_fingerprint.h - RF fingerprinting API
 *
 * A "fingerprint" is a compact representation of the local RF environment:
 *   - Top-N strongest APs by BSSID hash and RSSI
 *   - Used for WiFi-based positioning without GPS
 *   - Traverses the protocol as packed binary (fingerprint_msg_t)
 *
 * Interview talking points:
 *   - WiFi fingerprinting exploits stable multipath propagation
 *   - BSSID hash (32-bit) vs full MAC (48-bit) saves 2 bytes per AP
 *   - TOP_N_RSSI = 16 balances accuracy vs message size
 *   - rf_is_weak_signal() is the key decision function for repeater activation
 */

#ifndef RF_FINGERPRINT_H
#define RF_FINGERPRINT_H

#include <Arduino.h>
#include "common.h"

struct rf_scan_result_t {
    char     ssid[33];
    uint8_t  bssid[6];
    rssi_t   rssi;
    uint8_t  channel;
    uint8_t  authmode;
};

struct rf_fingerprint_t {
    uint32_t bssid_hashes[TOP_N_RSSI];
    rssi_t   rssi_values[TOP_N_RSSI];
    uint8_t  count;
    uint32_t timestamp;
};

uint32_t rf_hash_bssid(const uint8_t *bssid);
void rf_build_fingerprint(const rf_scan_result_t *results, uint8_t count, rf_fingerprint_t *fp);
void rf_fingerprint_to_msg(const rf_fingerprint_t *fp, battery_pct_t battery, mobile_state_t state, fingerprint_msg_t *msg);
wifi_quality_t rf_classify_quality(rssi_t avg_rssi);
bool rf_is_weak_signal(const rf_fingerprint_t *fp);
rssi_t rf_average_top_n(const rf_fingerprint_t *fp, uint8_t n);
uint8_t rf_count_valid(const rf_fingerprint_t *fp);
rssi_t rf_average_valid_top_n(const rf_fingerprint_t *fp, uint8_t n);
void rf_get_top_n(rf_scan_result_t *results, uint8_t total, rf_scan_result_t *top_n, uint8_t n);
void rf_print_fingerprint(const rf_fingerprint_t *fp);

#endif
