CREATE TABLE entries (
  id BIGSERIAL PRIMARY KEY,
  timestamp BIGINT NOT NULL,
  bssid_hash BIGINT NOT NULL,
  hour INT NOT NULL,
  day INT NOT NULL,
  rssi INT NOT NULL,
  is_weak BOOLEAN NOT NULL DEFAULT false,
  device TEXT NOT NULL DEFAULT 'base',
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE metrics (
  id INT PRIMARY KEY DEFAULT 1,
  data JSONB NOT NULL,
  received_at BIGINT NOT NULL
);

CREATE TABLE metrics_history (
  id BIGSERIAL PRIMARY KEY,
  data JSONB NOT NULL,
  received_at BIGINT NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX idx_entries_created ON entries(created_at DESC);
CREATE INDEX idx_entries_bssid ON entries(bssid_hash);
CREATE INDEX idx_metrics_history_created ON metrics_history(created_at DESC);

-- NOTE: the live worker (index.js) does NOT use SQL — it stores a versioned
-- JSON document in Cloudflare KV ("entries") so each report is exactly one
-- KV read + one KV write, well inside the free-tier budget. That document
-- folds every sample into LIFETIME aggregates that never expire:
--   * map   : per-(AP hash, hour)  running sum/min/max + counts
--   * dist  : per-(distance bucket, hour) running sum/min/max + counts
--   * meta  : lifetime total / weak / last_update counters
--   * recent: bounded raw ring (~6000) purely for the recent-events table
-- The raw "last 5000 entries", "last 200 metrics" caps therefore no longer
-- apply to the heatmaps — those accumulate every observation ever recorded.
-- Keep only last 2000 metrics_history rows (trend chart source).
-- Keep only last 50000 improvement samples.
