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

async function handleReport(request, env) {
  const data = await request.json();
  const now = Date.now();

  /* Heartbeat/status blob ({"total":...,"weak":...}) — this carries the
   * device's LOCAL map count which must not override the server store.
   * We acknowledge it without any KV write: /api/status derives totals
   * from the authoritative entries key on read, so a node in repeater
   * mode reporting 0 cannot clobber history. */
  if (!Array.isArray(data) && data.bssid_hash == null && data.total != null) {
    return new Response(JSON.stringify({ ok: true, status: 'heartbeat' }), {
      headers: { ...corsHeaders, 'Content-Type': 'application/json' },
    });
  }

  const items = Array.isArray(data) ? data : [data];
  const entries = items.map(d => normalizeEntry(d, now, 'base'));

  /* Append to the entries store. This is the ONLY write a report triggers —
   * status is computed on read, keeping us inside the free-tier KV budget
   * (1,000 writes/day). */
  let map = [];
  const existing = await env.RF_MAP.get('entries');
  if (existing) {
    try { map = JSON.parse(existing); } catch (e) { map = []; }
  }
  map.push(...entries);
  if (map.length > 5000) {
    map = map.slice(-5000);
  }
  await env.RF_MAP.put('entries', JSON.stringify(map));

  return new Response(JSON.stringify({ ok: true, count: entries.length, total: map.length }), {
    headers: { ...corsHeaders, 'Content-Type': 'application/json' },
  });
}

async function handleGetMap(env) {
  const data = await env.RF_MAP.get('entries');
  const entries = data ? JSON.parse(data) : [];

  const bssidMap = {};
  for (const e of entries) {
    const key = `${e.bssid_hash}_${e.hour}`;
    if (!bssidMap[key]) {
      bssidMap[key] = {
        bssid_hash: e.bssid_hash,
        hour: e.hour,
        rssi_values: [],
        ring_values: [],
        is_weak: false,
      };
    }
    bssidMap[key].rssi_values.push(e.rssi);
    if (e.ring != null) bssidMap[key].ring_values.push(e.ring);
    if (e.is_weak) bssidMap[key].is_weak = true;
  }

  const aggregated = Object.values(bssidMap).map(v => ({
    bssid_hash: v.bssid_hash,
    hour: v.hour,
    rssi_avg: Math.round(v.rssi_values.reduce((a, b) => a + b, 0) / v.rssi_values.length),
    rssi_min: Math.min(...v.rssi_values),
    rssi_max: Math.max(...v.rssi_values),
    ring_avg: v.ring_values.length
      ? Math.round(v.ring_values.reduce((a, b) => a + b, 0) / v.ring_values.length)
      : null,
    sample_count: v.rssi_values.length,
    is_weak: v.is_weak,
  }));

  return new Response(JSON.stringify({ entries: aggregated, total: entries.length }), {
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
  const data = await env.RF_MAP.get('entries');
  const entries = data ? JSON.parse(data) : [];

  /* Accumulate averages over time: bucket = (distance ring, hour). */
  const hourMap = {};
  for (let h = 0; h < 24; h++) hourMap[h] = {};
  const ringTotals = {};
  let totalSamples = 0;
  let weakSamples = 0;

  for (const e of entries) {
    if (e.rssi == null) continue;
    const ring = e.ring != null ? e.ring : ringFromRssi(e.rssi);
    const hour = (e.hour != null) ? e.hour : (e.timestamp ? new Date(e.timestamp).getHours() : 0);
    if (hour < 0 || hour > 23) continue;

    const bucketIdx = DISTANCE_BUCKETS.findIndex(b => ring >= b.min && ring <= b.max);
    if (bucketIdx < 0) continue;

    const key = `${bucketIdx}_${hour}`;
    if (!hourMap[hour][bucketIdx]) {
      hourMap[hour][bucketIdx] = { sum: 0, count: 0, max: -128, min: 0 };
    }
    const c = hourMap[hour][bucketIdx];
    c.sum += e.rssi;
    c.count++;
    if (e.rssi > c.max) c.max = e.rssi;
    if (e.rssi < c.min) c.min = e.rssi;

    if (!ringTotals[bucketIdx]) ringTotals[bucketIdx] = { sum: 0, count: 0 };
    ringTotals[bucketIdx].sum += e.rssi;
    ringTotals[bucketIdx].count++;
    totalSamples++;
    if (e.is_weak || e.rssi < -75) weakSamples++;
  }

  const cells = [];
  for (let h = 0; h < 24; h++) {
    for (let b = 0; b < DISTANCE_BUCKETS.length; b++) {
      const c = hourMap[h][b];
      if (!c || !c.count) continue;
      cells.push({
        bucket: b,
        bucket_label: DISTANCE_BUCKETS[b].label,
        hour: h,
        rssi_avg: Math.round(c.sum / c.count),
        rssi_min: c.min,
        rssi_max: c.max,
        sample_count: c.count,
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
  const data = await env.RF_MAP.get('entries');
  const entries = data ? JSON.parse(data) : [];
  const last_update = entries.length ? Math.max(...entries.map(e => e.timestamp || 0)) : 0;
  const weak_count = entries.filter(e => e.is_weak).length;

  const status = {
    last_update,
    total_entries: entries.length,
    weak_count,
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
  const data = await env.RF_MAP.get('entries');
  const entries = data ? JSON.parse(data) : [];
  const recent = entries.slice(-limit);
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
  if (history.length > 200) {
    history = history.slice(-200);
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
  if (hist.length > 5000) hist = hist.slice(-5000);
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