/*
 * rf_fingerprint.cpp - RF fingerprinting engine for BLUE
 *
 * Builds compact "fingerprints" from WiFi scan results to identify
 * the mobile unit's location without GPS.
 *
 * Algorithm:
 *   1. WiFi.scanNetworks() returns all visible APs
 *   2. Sort by RSSI (strongest first) using selection sort O(n*k)
 *   3. Take top-N APs (TOP_N_RSSI = 16) to bound message size
 *   4. Hash each BSSID (djb2) to 32-bit for bandwidth savings
 *   5. Pair {bssid_hash, rssi} as the location fingerprint
 *
 * Interview talking points:
 *   - dBm is logarithmic: -70 dBm = 100x weaker than -50 dBm
 *   - Top-N selection (not full sort) is O(n*k) vs O(n*log(n))
 *   - BSSID hashing reduces fingerprint_msg from 160+ bytes to ~80 bytes
 *   - Weak signal threshold (-75 dBm) is empirically derived from
 *     measuring TCP retransmission rates at various RSSI levels
 *   - RF fingerprints are surprisingly stable for a given location
 *     because they depend on physical AP positions and building geometry
 */

#include "rf_fingerprint.h"

uint32_t rf_hash_bssid(const uint8_t *bssid) {
    uint32_t hash = 5381;
    for (int i = 0; i < 6; i++) {
        hash = ((hash << 5) + hash) + bssid[i];
    }
    return hash;
}

void rf_get_top_n(rf_scan_result_t *results, uint8_t total, rf_scan_result_t *top_n, uint8_t n) {
    bool used[32] = {false};
    for (uint8_t i = 0; i < n && i < total; i++) {
        rssi_t best = -128;
        int best_idx = -1;
        for (uint8_t j = 0; j < total; j++) {
            if (!used[j] && results[j].rssi > best) {
                best = results[j].rssi;
                best_idx = j;
            }
        }
        if (best_idx >= 0) {
            top_n[i] = results[best_idx];
            used[best_idx] = true;
        }
    }
}

void rf_build_fingerprint(const rf_scan_result_t *results, uint8_t count, rf_fingerprint_t *fp) {
    rf_scan_result_t sorted[32];
    uint8_t n = count < 32 ? count : 32;
    memcpy(sorted, results, n * sizeof(rf_scan_result_t));

    rf_scan_result_t top[32];
    memset(top, 0, sizeof(top));
    uint8_t top_n = n < TOP_N_RSSI ? n : TOP_N_RSSI;
    rf_get_top_n(sorted, n, top, top_n);

    fp->count = top_n;
    fp->timestamp = millis();
    for (uint8_t i = 0; i < top_n; i++) {
        fp->bssid_hashes[i] = rf_hash_bssid(top[i].bssid);
        fp->rssi_values[i] = top[i].rssi;
    }
    for (uint8_t i = top_n; i < TOP_N_RSSI; i++) {
        fp->bssid_hashes[i] = 0;
        fp->rssi_values[i] = -128;
    }
}

void rf_fingerprint_to_msg(const rf_fingerprint_t *fp, battery_pct_t battery, mobile_state_t state, fingerprint_msg_t *msg) {
    msg->msg_type = MSG_FINGERPRINT;
    msg->timestamp = fp->timestamp;
    memcpy(msg->rssi_values, fp->rssi_values, TOP_N_RSSI);
    memcpy(msg->bssid_hashes, fp->bssid_hashes, TOP_N_RSSI * sizeof(uint32_t));
    msg->scan_count = fp->count;
    msg->battery_pct = battery;
    msg->local_state = state;
    msg->local_rssi_avg = rf_average_top_n(fp, 3);
    msg->sequence_num++;
}

wifi_quality_t rf_classify_quality(rssi_t avg_rssi) {
    if (avg_rssi >= -50) return WIFI_QUAL_EXCELLENT;
    if (avg_rssi >= -60) return WIFI_QUAL_GOOD;
    if (avg_rssi >= -70) return WIFI_QUAL_FAIR;
    if (avg_rssi >= -80) return WIFI_QUAL_POOR;
    return WIFI_QUAL_DEAD;
}

/* -128 dBm is the ESP32 "no data"/invalid placeholder; a scan that only
 * returns -128 readings (e.g. momentary radio state) is NOT a weak-signal
 * observation and must never be classified as a dead zone. */
bool rf_is_weak_signal(const rf_fingerprint_t *fp) {
    if (rf_count_valid(fp) == 0) return false;
    return rf_average_valid_top_n(fp, 3) < WEAK_RSSI_THRESHOLD;
}

uint8_t rf_count_valid(const rf_fingerprint_t *fp) {
    uint8_t valid = 0;
    for (uint8_t i = 0; i < fp->count; i++) {
        if (fp->rssi_values[i] > -128) valid++;
    }
    return valid;
}

/* Average the strongest n valid APs. Returns -128 if there is nothing valid
 * (callers should check rf_count_valid() first). */
rssi_t rf_average_valid_top_n(const rf_fingerprint_t *fp, uint8_t n) {
    uint8_t count = n < fp->count ? n : fp->count;
    int32_t sum = 0;
    uint8_t used = 0;
    for (uint8_t i = 0; i < count; i++) {
        if (fp->rssi_values[i] > -128) {
            sum += fp->rssi_values[i];
            used++;
        }
    }
    if (used == 0) return -128;
    return (rssi_t)(sum / used);
}

/* Average the strongest n APs (legacy: includes invalid -128 readings).
 * Kept for scan logging where fp->count is always real. */
rssi_t rf_average_top_n(const rf_fingerprint_t *fp, uint8_t n) {
    if (fp->count == 0) return -128;
    uint8_t count = n < fp->count ? n : fp->count;
    int32_t sum = 0;
    for (uint8_t i = 0; i < count; i++) {
        sum += fp->rssi_values[i];
    }
    return (rssi_t)(sum / count);
}

void rf_print_fingerprint(const rf_fingerprint_t *fp) {
    Serial.printf("[FINGERPRINT] %u APs (%u valid), avg RSSI: %d dBm, weak: %s\n",
                  fp->count, rf_count_valid(fp),
                  rf_average_top_n(fp, 3),
                  rf_is_weak_signal(fp) ? "YES" : "no");
    for (uint8_t i = 0; i < fp->count; i++) {
        Serial.printf("  [%u] hash=0x%08lX rssi=%d dBm\n",
                      i, fp->bssid_hashes[i], fp->rssi_values[i]);
    }
}
