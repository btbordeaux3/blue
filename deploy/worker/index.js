const corsHeaders = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
  'Access-Control-Allow-Headers': 'Content-Type',
};

/* RSSI -> distance (meters) via indoor path-loss at 2.4GHz.
 * Reference: -30 dBm at 1 m, path-loss exponent ~2.8 (walls/furniture). */
function rssiToMeters(rssi) {
  const n = 2.8;
  if (rssi >= -30) return 0.5;
  return Math.round(Math.pow(10, (-30 - rssi) / (10 * n)));
}
function ringFromRssi(rssi) {
  const m = rssiToMeters(rssi);
  return Math.max(0, Math.min(20, m)); // ring index == meters, capped 20 m
}

/* Normalize a single fingerprint entry, accepting both the compact
 * firmware keys and the legacy full keys. */
function normalizeEntry(d, now, deviceDefault) {
  const rssi = (d.r == null) ? d.rssi : d.r;
  const hour = (d.hr == null) ? d.hour : d.hr;
  const day = (d.d == null) ? d.day : d.d;
  return {
    timestamp: d.timestamp || now,
    bssid_hash: d.h || d.bssid_hash,
    hour: hour,
    day: day,
    rssi: rssi,
    is_weak: rssi < -75,
    ring: (d.ring == null) ? ringFromRssi(rssi) : d.ring,
    device: d.device || deviceDefault,
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
  const snapshot = { ...data, received_at: now };

  /* Single-write history (latest metric == last history item), so a
   * 10-minute firmware cadence stays far inside the KV free tier. */
  let history = [];
  const histData = await env.RF_MAP.get('metrics_history');
  if (histData) {
    try { history = JSON.parse(histData); } catch (e) { history = []; }
  }
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
  ]);

  return new Response(JSON.stringify({ ok: true, reset: true }), {
    headers: { ...corsHeaders, 'Content-Type': 'application/json' },
  });
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