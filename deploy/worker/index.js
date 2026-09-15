const corsHeaders = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
  'Access-Control-Allow-Headers': 'Content-Type',
};

/* RSSI -> distance (meters) via indoor path-loss at 2.4GHz.
 * Reference: -40 dBm at 1 m, path-loss exponent 3.3 (walled-indoor).
 * Sanity: -55 ~ 3m, -65 ~ 6m, -75 ~ 12m, -85 ~ 2x the cap. The old
 * constants (-30@1m, n=2.8) estimated sub-meter distances for most real
 * readings, dumping every AP into a blob at the base station. */
const RSSI_REF_DBM = -40;
const PATH_LOSS_N = 3.3;
function rssiToMeters(rssi) {
  if (rssi >= RSSI_REF_DBM) return 1;
  return Math.round(Math.pow(10, (RSSI_REF_DBM - rssi) / (10 * PATH_LOSS_N)));
}
function ringFromRssi(rssi) {
  const m = rssiToMeters(rssi);
  return Math.max(0, Math.min(20, m)); // ring index == meters, capped 20 m
}

/* Normalize a single fingerprint entry, accepting both the compact
 * firmware keys and the legacy full keys. `v` tags the observer: 0 = the
 * stationary base scan (map source of truth), 1 = a mobile/handheld node. */
const DEVICE_BASE = 'base';
const DEVICE_MOBILE = 'mobile';
function normalizeEntry(d, now, deviceDefault) {
  const rssi = (d.r == null) ? d.rssi : d.r;
  const hour = (d.hr == null) ? d.hour : d.hr;
  const day = (d.d == null) ? d.day : d.d;
  let device = deviceDefault;
  if (d.v === 1) device = DEVICE_MOBILE;
  else if (d.v === 0) device = DEVICE_BASE;
  else if (d.device) device = d.device;
  return {
    timestamp: d.timestamp || now,
    bssid_hash: d.h || d.bssid_hash,
    hour: hour,
    day: day,
    rssi: rssi,
    is_weak: rssi < -75,
    ring: (d.ring == null) ? ringFromRssi(rssi) : d.ring,
    device,
  };
}

/* =========================================================================
 * Persistent store: aggregates keep the map alive forever, raw stays small.
 *
 * A single KV key "entries" holds a versioned document so a report still
 * costs exactly ONE KV read + ONE KV write (Cloudflare KV free tier allows
 * ~1,000 writes/day and the firmware posts every 2 minutes, so <=1 write
 * per report is a hard requirement). The document accumulates the things
 * that MUST live forever as running sums (per-AP-per-hour signal, per
 * distance-bucket-per-hour signal, and lifetime totals), while the raw
 * "recent" ring stays bounded purely so /api/history and the recent-events
 * table have something cheap to read.
 *
 *   db.v       = schema version (2)
 *   db.recent  = bounded ring of normalized raw entries (for recent events)
 *   db.map     = lifetime rollup  key `${bssid_hash}_${hour}`
 *                { s:sum, c:count, mn:min, mx:max, rs:ringSum, rc:ringCount,
 *                  wk:weakCount, t:lastTs }
 *   db.dist    = lifetime rollup  key `${bucketIdx}_${hour}`
 *                { s:sum, c:count, mn:min, mx:max, wk:weakCount }
 *   db.meta    = { total, weak, last_update }  (lifetime counters)
 *
 * Before v2 the key held a plain array capped at 5000 raw entries ("Total
 * Entries" sat pinned at 5000 forever). loadDb() folds any legacy array in
 * on the fly, so the already-collected history is preserved and counted.
 * ========================================================================= */

const RECENT_MAX = 6000;      /* bounded raw ring for /api/history, recent events */
const MAP_MAX_KEYS = 20000;   /* cap on distinct (ap, hour) rollup cells, FIFO */

function newDb() {
  return { v: 2, recent: [], map: {}, dist: {}, meta: { total: 0, weak: 0, last_update: 0 } };
}

/* Fold one normalized entry into every lifetime aggregate. */
function foldEntry(db, e) {
  db.meta.total++;
  if (e.is_weak) db.meta.weak++;
  const ts = e.timestamp || 0;
  if (ts > db.meta.last_update) db.meta.last_update = ts;

  const hour = (e.hour != null) ? e.hour : (ts ? new Date(ts).getHours() : 0);
  if (hour < 0 || hour > 23) return;
  const rssi = e.rssi;
  if (rssi == null) return;
  const ring = e.ring != null ? e.ring : ringFromRssi(rssi);

  /* Per-AP-per-hour accumulation (drives /api/map charts). */
  if (e.bssid_hash != null) {
    const k = e.bssid_hash + '_' + hour;
    let g = db.map[k];
    if (!g) { g = { s: 0, c: 0, mn: 0, mx: -128, rs: 0, rc: 0, wk: 0, t: 0 }; db.map[k] = g; }
    g.s += rssi; g.c++;
    if (g.c === 1 || rssi < g.mn) g.mn = rssi;
    if (rssi > g.mx) g.mx = rssi;
    if (e.ring != null) { g.rs += e.ring; g.rc++; }
    if (e.is_weak) g.wk++;
    if (ts > g.t) g.t = ts;
  }

  /* Distance-bucket-per-hour accumulation (drives /api/distance heatmap). */
  const bIdx = DISTANCE_BUCKETS.findIndex(b => ring >= b.min && ring <= b.max);
  if (bIdx >= 0) {
    const dk = bIdx + '_' + hour;
    let d = db.dist[dk];
    if (!d) { d = { s: 0, c: 0, mn: 0, mx: -128, wk: 0 }; db.dist[dk] = d; }
    d.s += rssi; d.c++;
    if (d.c === 1 || rssi < d.mn) d.mn = rssi;
    if (rssi > d.mx) d.mx = rssi;
    if (e.is_weak) d.wk++;
  }
}

/* Drop the oldest-touched rollup cells if the map ever grows too big, so
 * the JSON document (and every GET response) stays bounded. */
function pruneMap(db) {
  const keys = Object.keys(db.map);
  if (keys.length <= MAP_MAX_KEYS) return;
  keys.sort((a, b) => (db.map[a].t || 0) - (db.map[b].t || 0));
  for (let i = 0; i < keys.length - MAP_MAX_KEYS; i++) delete db.map[keys[i]];
}

/* Read the store. Legacy v1 (plain raw array) is folded to v2 on the fly
 * without writing, so GETs are never destructive. */
async function loadDb(env) {
  const data = await env.RF_MAP.get('entries');
  if (!data) return newDb();
  let raw;
  try { raw = JSON.parse(data); } catch (e) { return newDb(); }
  if (Array.isArray(raw)) {
    /* Legacy v1 store: a plain list of raw entries in compact firmware
     * keys ({h,hr,d,r}) — normalize each before folding so the lifetime
     * aggregates get the RSSI/hour data, not just the counters. */
    const db = newDb();
    const now = Date.now();
    for (const e of raw) foldEntry(db, normalizeEntry(e, now, DEVICE_BASE));
    db.recent = raw.slice(-RECENT_MAX).map(e => normalizeEntry(e, e.timestamp || now, DEVICE_BASE));
    return db;
  }
  if (raw && raw.v === 2 && raw.map && raw.meta) return raw;
  return newDb();
}

async function saveDb(env, db) {
  await env.RF_MAP.put('entries', JSON.stringify(db));
}

async function handleReport(request, env) {
  const data = await request.json();
  const now = Date.now();

  /* Heartbeat/status blob ({"total":...,"weak":...}) — this carries the
   * device's LOCAL map count which must not override the server store.
   * We acknowledge it without any KV write: /api/status derives totals
   * from the authoritative store on read, so a node in repeater mode
   * reporting 0 cannot clobber history. */
  if (!Array.isArray(data) && data.bssid_hash == null && data.total != null) {
    return new Response(JSON.stringify({ ok: true, status: 'heartbeat' }), {
      headers: { ...corsHeaders, 'Content-Type': 'application/json' },
    });
  }

  const items = Array.isArray(data) ? data : [data];
  const entries = items.map(d => normalizeEntry(d, now, 'base'));

  /* One read + one write per report: fold new samples into the lifetime
   * aggregates and keep the recent ring bounded. NO entry is ever thrown
   * away without being counted first. */
  const db = await loadDb(env);
  for (const e of entries) foldEntry(db, e);
  db.recent.push(...entries);
  if (db.recent.length > RECENT_MAX) {
    db.recent = db.recent.slice(-RECENT_MAX);
  }
  pruneMap(db);
  await saveDb(env, db);

  return new Response(JSON.stringify({ ok: true, count: entries.length, total: db.meta.total }), {
    headers: { ...corsHeaders, 'Content-Type': 'application/json' },
  });
}

async function handleGetMap(env) {
  const db = await loadDb(env);
  const aggregated = Object.keys(db.map).map(k => {
    const sep = k.indexOf('_');
    const g = db.map[k];
    return {
      bssid_hash: Number(k.slice(0, sep)),
      hour: Number(k.slice(sep + 1)),
      rssi_avg: Math.round(g.s / g.c),
      rssi_min: g.mn,
      rssi_max: g.mx,
      ring_avg: g.rc ? Math.round(g.rs / g.rc) : null,
      sample_count: g.c,
      is_weak: g.wk > 0,
    };
  });

  return new Response(JSON.stringify({ entries: aggregated, total: db.meta.total, weak: db.meta.weak }), {
    headers: { ...corsHeaders, 'Content-Type': 'application/json' },
  });
}

/* Distance buckets (meters from base, derived from RSSI path loss). The
 * website heatmap rows map onto these so the accumulated averages are always
 * grouped by how far away the router is, not by which router it is. */
const DISTANCE_BUCKETS = [
  { label: '0-2m', min: 0,  max: 2 },
  { label: '3-4m', min: 3,  max: 4 },
  { label: '5-6m', min: 5,  max: 6 },
  { label: '7-9m', min: 7,  max: 9 },
  { label: '10-13m', min: 10, max: 13 },
  { label: '14-20m', min: 14, max: 20 },
];

async function handleGetDistance(env) {
  const db = await loadDb(env);

  const hourMap = {};
  for (let h = 0; h < 24; h++) hourMap[h] = {};
  const ringTotals = {};
  let totalSamples = 0;
  let weakSamples = 0;

  for (const k of Object.keys(db.dist)) {
    const sep = k.indexOf('_');
    const b = Number(k.slice(0, sep));
    const h = Number(k.slice(sep + 1));
    const d = db.dist[k];
    if (!hourMap[h][b]) hourMap[h][b] = d;
    if (!ringTotals[b]) ringTotals[b] = { sum: 0, count: 0, wk: 0 };
    ringTotals[b].sum += d.s;
    ringTotals[b].count += d.c;
    ringTotals[b].wk += d.wk;
    totalSamples += d.c;
    weakSamples += d.wk;
  }

  const cells = [];
  for (let h = 0; h < 24; h++) {
    for (let b = 0; b < DISTANCE_BUCKETS.length; b++) {
      const d = hourMap[h][b];
      if (!d || !d.c) continue;
      cells.push({
        bucket: b,
        bucket_label: DISTANCE_BUCKETS[b].label,
        hour: h,
        rssi_avg: Math.round(d.s / d.c),
        rssi_min: d.mn,
        rssi_max: d.mx,
        sample_count: d.c,
      });
    }
  }

  const rings = DISTANCE_BUCKETS.map((b, i) => ({
    bucket: i,
    label: b.label,
    rssi_avg: ringTotals[i] ? Math.round(ringTotals[i].sum / ringTotals[i].count) : null,
    sample_count: ringTotals[i] ? ringTotals[i].count : 0,
  }));

  return new Response(JSON.stringify({
    cells,
    rings,
    buckets: DISTANCE_BUCKETS,
    total_samples: totalSamples,
    weak_samples: weakSamples,
  }), {
    headers: { ...corsHeaders, 'Content-Type': 'application/json' },
  });
}

async function handleGetStatus(env) {
  const db = await loadDb(env);

  const status = {
    last_update: db.meta.last_update,
    total_entries: db.meta.total,
    weak_count: db.meta.weak,
  };

  const dec = await env.RF_MAP.get('decision');
  if (dec) {
    status.decision = JSON.parse(dec);
  } else {
    status.decision = {
      active: false,
      source: 0,
      source_name: 'none',
      confidence: 0,
      mobile_state: 0,
      base_activation_count: 0,
      updated: 0,
      model: { trained: false, samples: 0, pos: 0, neg: 0 },
    };
  }
  return new Response(JSON.stringify(status), {
    headers: { ...corsHeaders, 'Content-Type': 'application/json' },
  });
}

async function handleDecision(request, env) {
  const data = await request.json();
  const now = Date.now();
  const decision = { ...data, updated: now };
  await env.RF_MAP.put('decision', JSON.stringify(decision));
  return new Response(JSON.stringify({ ok: true }), {
    headers: { ...corsHeaders, 'Content-Type': 'application/json' },
  });
}

async function handleGetHistory(url, env) {
  const limit = parseInt(url.searchParams.get('limit') || '100');
  const db = await loadDb(env);
  const recent = db.recent.slice(-limit);
  return new Response(JSON.stringify({ entries: recent }), {
    headers: { ...corsHeaders, 'Content-Type': 'application/json' },
  });
}

async function handleMetrics(request, env) {
  const data = await request.json();
  const now = Date.now();

  /* Compute per-minute activity rates against the previous snapshot, so the
   * dashboard can show "events/min" and "scans/min" without a second store
   * and without waiting for the next poll cycle. */
  let history = [];
  const histData = await env.RF_MAP.get('metrics_history');
  if (histData) {
    try { history = JSON.parse(histData); } catch (e) { history = []; }
  }
  const prev = history.length ? history[history.length - 1] : null;
  let rate = null;
  if (prev && prev.received_at) {
    const minutes = Math.max((now - prev.received_at) / 60000, 0.5);
    rate = {
      events_per_min: Math.round(((data.events?.logged || 0) - (prev.events?.logged || 0)) / minutes),
      scans_per_min: Math.round(((data.scan?.count || 0) - (prev.scan?.count || 0)) / minutes),
      espnow_tx_per_min: Math.round(((data.espnow?.tx || 0) - (prev.espnow?.tx || 0)) / minutes),
    };
  }

  const snapshot = { ...data, device: data.device || DEVICE_BASE, received_at: now, rate };

  /* Single-write history (latest metric == last history item), so a
   * 10-minute firmware cadence stays far inside the KV free tier. */
  history.push(snapshot);
  if (history.length > 2000) {
    history = history.slice(-2000);
  }
  await env.RF_MAP.put('metrics_history', JSON.stringify(history));

  return new Response(JSON.stringify({ ok: true }), {
    headers: { ...corsHeaders, 'Content-Type': 'application/json' },
  });
}

async function handleGetMetrics(env) {
  const data = await env.RF_MAP.get('metrics_history');
  const history = data ? JSON.parse(data) : [];
  const metrics = history.length ? history[history.length - 1] : null;
  return new Response(JSON.stringify(metrics || { error: 'no metrics yet' }), {
    headers: { ...corsHeaders, 'Content-Type': 'application/json' },
  });
}

async function handleGetMetricsHistory(env) {
  const data = await env.RF_MAP.get('metrics_history');
  const history = data ? JSON.parse(data) : [];
  return new Response(JSON.stringify({ entries: history }), {
    headers: { ...corsHeaders, 'Content-Type': 'application/json' },
  });
}

async function handleReset(request, env) {
  const caller = new URL(request.url);
  if (caller.searchParams.get('key') !== 'blue-reset') {
    return new Response(JSON.stringify({ error: 'forbidden' }), {
      status: 403,
      headers: { ...corsHeaders, 'Content-Type': 'application/json' },
    });
  }

  await Promise.all([
    env.RF_MAP.delete('entries'),
    env.RF_MAP.delete('status'),
    env.RF_MAP.delete('decision'),
    env.RF_MAP.delete('metrics'),
    env.RF_MAP.delete('metrics_history'),
    env.RF_MAP.delete('improvement'),
  ]);

  return new Response(JSON.stringify({ ok: true, reset: true }), {
    headers: { ...corsHeaders, 'Content-Type': 'application/json' },
  });
}

/* Repeater-improvement samples, posted by the host esp-wifi-agent while it is
 * attached to the extender. Each sample is
 *   improvement_dbm = signal_to_extender - signal_to_router_direct
 * so a positive value means the extender is helping. The worker only appends
 * (one KV read + write per POST) and computes the stats on read, keeping us
 * inside the free-tier KV budget. */
async function handleImprovement(request, env) {
  const data = await request.json();
  const now = Date.now();
  const sample = {
    improvement_dbm: Math.round(data.improvement_dbm || 0),
    ext_dbm: data.ext_dbm != null ? Math.round(data.ext_dbm) : null,
    direct_dbm: data.direct_dbm != null ? Math.round(data.direct_dbm) : null,
    ts: now,
  };

  let hist = [];
  const existing = await env.RF_MAP.get('improvement');
  if (existing) {
    try { hist = JSON.parse(existing); } catch (e) { hist = []; }
  }
  hist.push(sample);
  if (hist.length > 50000) hist = hist.slice(-50000);
  await env.RF_MAP.put('improvement', JSON.stringify(hist));

  return new Response(JSON.stringify({ ok: true, count: hist.length }), {
    headers: { ...corsHeaders, 'Content-Type': 'application/json' },
  });
}

async function handleGetImprovement(env) {
  const data = await env.RF_MAP.get('improvement');
  const hist = data ? JSON.parse(data) : [];
  if (!hist.length) {
    return new Response(JSON.stringify({
      count: 0,
      avg_improvement_dbm: null,
      entries: [],
    }), { headers: { ...corsHeaders, 'Content-Type': 'application/json' } });
  }
  const avg = Math.round(
    hist.reduce((a, s) => a + s.improvement_dbm, 0) / hist.length);
  const recent = hist.slice(-20).reverse();
  return new Response(JSON.stringify({
    count: hist.length,
    avg_improvement_dbm: avg,
    total_improvement_dbm: hist.reduce((a, s) => a + s.improvement_dbm, 0),
    entries: recent,
  }), { headers: { ...corsHeaders, 'Content-Type': 'application/json' } });
}

export default {
  async fetch(request, env) {
    if (request.method === 'OPTIONS') {
      return new Response(null, { headers: corsHeaders });
    }

    const url = new URL(request.url);

    try {
      if (url.pathname === '/api/reset' && request.method === 'POST') {
        return await handleReset(request, env);
      }
      if (url.pathname === '/api/report' && request.method === 'POST') {
        return await handleReport(request, env);
      }
      if (url.pathname === '/api/decision' && request.method === 'POST') {
        return await handleDecision(request, env);
      }
      if (url.pathname === '/api/map' && request.method === 'GET') {
        return await handleGetMap(env);
      }
      if (url.pathname === '/api/distance' && request.method === 'GET') {
        return await handleGetDistance(env);
      }
      if (url.pathname === '/api/status' && request.method === 'GET') {
        return await handleGetStatus(env);
      }
      if (url.pathname === '/api/history' && request.method === 'GET') {
        return await handleGetHistory(url, env);
      }
      if (url.pathname === '/api/metrics' && request.method === 'POST') {
        return await handleMetrics(request, env);
      }
      if (url.pathname === '/api/metrics' && request.method === 'GET') {
        return await handleGetMetrics(env);
      }
      if (url.pathname === '/api/metrics/history' && request.method === 'GET') {
        return await handleGetMetricsHistory(env);
      }
      if (url.pathname === '/api/improvement' && request.method === 'POST') {
        return await handleImprovement(request, env);
      }
      if (url.pathname === '/api/improvement' && request.method === 'GET') {
        return await handleGetImprovement(env);
      }
    } catch (e) {
      console.error(`[rf-map-api] error on ${request.method} ${url.pathname}: ${e.stack || e.message}`);
      return new Response(JSON.stringify({ error: e.message }), {
        status: 500,
        headers: { ...corsHeaders, 'Content-Type': 'application/json' },
      });
    }

    return new Response('Not found', { status: 404, headers: corsHeaders });
  },
};