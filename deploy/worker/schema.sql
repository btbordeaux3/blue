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

-- Keep only last 5000 entries (trim periodically or use a trigger)
-- Keep only last 100 metrics_history rows
