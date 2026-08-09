// Telemetry dashboard: per-site max/min for each sensor over a chosen window.
// Queries InfluxDB server-side so the token never reaches the browser.
const http = require('node:http');
const net = require('node:net');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');

const { INFLUX_URL, INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET, GRAFANA_URL } = process.env;
for (const [k, v] of Object.entries({ INFLUX_URL, INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET, GRAFANA_URL })) {
  if (!v) {
    console.error(`missing required env var ${k}`);
    process.exit(1);
  }
}

const PORT = process.env.PORT || 8080;
const FIELDS = ['temp', 'humidity', 'pressure'];
// Site names come from MQTT topic parsing (sensors/<site>/<room>), so they can
// contain dots — "poble.sec" is a real site. Quotes and backslashes stay
// excluded because `site` is interpolated into a Flux string literal; the
// membership check against listSites() below is the primary guard, this is
// defence in depth.
const SITE_RE = /^[A-Za-z0-9._-]+$/;
// Allowlist: the ?range token maps to a Flux relative start. Nothing else
// reaches the query, so range can't be injected.
const RANGES = { '6h': '-6h', '24h': '-24h', '7d': '-7d' };
// Same windows as milliseconds, for turning a range key into the absolute
// cut-off timestamp the Influx delete API wants.
const RANGE_MS = { '6h': 6 * 3600e3, '24h': 24 * 3600e3, '7d': 7 * 24 * 3600e3 };
const INDEX = fs.readFileSync(path.join(__dirname, 'public', 'index.html'));

// Admin (destructive deletes) is OPT-IN: with no ADMIN_PASSWORD set, the
// /api/admin/* routes refuse everything. A missing secret must fail closed —
// never silently expose data deletion. Deliberately not in the required-env
// loop above, so the dashboard still runs read-only without it.
const ADMIN_PASSWORD = process.env.ADMIN_PASSWORD || '';
const EPOCH = '1970-01-01T00:00:00Z';

// Failed-auth backoff, keyed by peer address. This dashboard is reachable from
// the public internet, so an unthrottled password check is a brute-force target.
const authFails = new Map();
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function peer(req) {
  return req.socket.remoteAddress || 'unknown';
}

function checkAdmin(req) {
  if (!ADMIN_PASSWORD) return false;
  const header = req.headers.authorization || '';
  if (!header.startsWith('Basic ')) return false;
  const decoded = Buffer.from(header.slice(6), 'base64').toString('utf8');
  const supplied = Buffer.from(decoded.slice(decoded.indexOf(':') + 1), 'utf8');
  const expected = Buffer.from(ADMIN_PASSWORD, 'utf8');
  // timingSafeEqual throws on length mismatch, so length is checked first —
  // that leaks the password's length and nothing else.
  return supplied.length === expected.length && crypto.timingSafeEqual(supplied, expected);
}

// How many points still exist for `site`, over all time. Used to prove a delete
// actually removed something — the Influx delete API returns 204 even when its
// predicate matches nothing, so "success" alone means very little.
async function sitePointCount(site) {
  const rows = await flux(
    `from(bucket: "${INFLUX_BUCKET}")\n` +
      `  |> range(start: 1970-01-01T00:00:00Z)\n` +
      `  |> filter(fn: (r) => r._measurement == "telemetry")\n` +
      `  |> filter(fn: (r) => r.site == "${site}")\n` +
      `  |> count()\n` +
      `  |> group()\n` +
      `  |> sum()`,
  );
  return rows.reduce((n, r) => n + (parseInt(r._value, 10) || 0), 0);
}

// Delete every telemetry point for `site` in [start, stop). `site` has already
// been checked against SITE_RE and listSites(), so it cannot escape the
// predicate's string literal.
async function influxDelete(site, start, stop) {
  const url =
    `${INFLUX_URL}/api/v2/delete` +
    `?org=${encodeURIComponent(INFLUX_ORG)}&bucket=${encodeURIComponent(INFLUX_BUCKET)}`;
  const res = await fetch(url, {
    method: 'POST',
    headers: { Authorization: `Token ${INFLUX_TOKEN}`, 'Content-Type': 'application/json' },
    body: JSON.stringify({
      start,
      stop,
      predicate: `_measurement="telemetry" AND site="${site}"`,
    }),
    signal: AbortSignal.timeout(30000),
  });
  if (!res.ok) {
    throw new Error(`influx delete ${res.status}: ${(await res.text()).slice(0, 200)}`);
  }
}

// Flux returns annotated CSV: '#' metadata lines, then a header, then rows.
// A blank line starts a new result table with its own header.
function parseCsv(text) {
  const rows = [];
  let header = null;
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line) {
      header = null;
      continue;
    }
    if (line.startsWith('#')) continue;
    const cells = line.split(',');
    if (!header) {
      header = cells;
      continue;
    }
    rows.push(Object.fromEntries(header.map((h, i) => [h, cells[i]])));
  }
  return rows;
}

async function flux(query) {
  const url = `${INFLUX_URL.replace(/\/$/, '')}/api/v2/query?org=${encodeURIComponent(INFLUX_ORG)}`;
  const res = await fetch(url, {
    method: 'POST',
    headers: {
      Authorization: `Token ${INFLUX_TOKEN}`,
      'Content-Type': 'application/vnd.flux',
      Accept: 'application/csv',
    },
    body: query,
    signal: AbortSignal.timeout(15000),
  });
  const body = await res.text();
  if (!res.ok) throw new Error(`influx ${res.status}: ${body.slice(0, 200)}`);
  return parseCsv(body);
}

// The single source of truth for "a site this dashboard can serve". Both
// /api/status (which fills the dropdown) and /api/summary's membership check
// go through here, so the UI can never offer a site the API would reject —
// that mismatch is exactly what made poble.sec unselectable.
async function listSites() {
  const rows = await flux(
    `import "influxdata/influxdb/schema"\n` +
      `schema.tagValues(bucket: "${INFLUX_BUCKET}", tag: "site")`,
  );
  const all = [...new Set(rows.map((r) => r._value).filter(Boolean))].sort();
  const usable = all.filter((s) => SITE_RE.test(s));
  for (const s of all) {
    if (!SITE_RE.test(s)) {
      // Loud, because the site is publishing but will be invisible in the UI.
      console.warn(`site ignored, unsupported characters in name: ${JSON.stringify(s)}`);
    }
  }
  return usable;
}

// Mean of each field over the selected window — the same window the max/min
// use, so changing the range selector moves every number on the page. `site` is
// validated against listSites() and `rangeFlux` against RANGES before reaching
// here, so neither can break out of the string literals below.
async function siteAverages(site, rangeFlux) {
  const fieldFilter = FIELDS.map((f) => `r._field == "${f}"`).join(' or ');
  const rows = await flux(
    `from(bucket: "${INFLUX_BUCKET}")\n` +
      `  |> range(start: ${rangeFlux})\n` +
      `  |> filter(fn: (r) => r._measurement == "telemetry")\n` +
      `  |> filter(fn: (r) => r.site == "${site}")\n` +
      `  |> filter(fn: (r) => ${fieldFilter})\n` +
      `  |> group(columns: ["_field"])\n` +
      `  |> mean()`,
  );
  const out = {};
  for (const r of rows) {
    if (FIELDS.includes(r._field) && r._value) {
      out[r._field] = Math.round(parseFloat(r._value) * 10) / 10;
    }
  }
  return out;
}

// Max and min of each field over the window, each with the timestamp it
// occurred. Flux max()/min() select the whole extreme row, so _time comes free.
// `site` is validated against listSites() and `rangeFlux` against RANGES before
// reaching here, so neither can break out of the string literals below.
async function siteExtremes(site, rangeFlux) {
  const fieldFilter = FIELDS.map((f) => `r._field == "${f}"`).join(' or ');
  const query = (agg) =>
    `from(bucket: "${INFLUX_BUCKET}")\n` +
    `  |> range(start: ${rangeFlux})\n` +
    `  |> filter(fn: (r) => r._measurement == "telemetry")\n` +
    `  |> filter(fn: (r) => r.site == "${site}")\n` +
    `  |> filter(fn: (r) => ${fieldFilter})\n` +
    `  |> group(columns: ["_field"])\n` +
    `  |> ${agg}()`;
  const [maxRows, minRows] = await Promise.all([flux(query('max')), flux(query('min'))]);
  const out = Object.fromEntries(FIELDS.map((f) => [f, { max: null, min: null }]));
  const fill = (rows, key) => {
    for (const r of rows) {
      if (FIELDS.includes(r._field) && r._value) {
        out[r._field][key] = { value: Math.round(parseFloat(r._value) * 10) / 10, time: r._time };
      }
    }
  };
  fill(maxRows, 'max');
  fill(minRows, 'min');
  return out;
}

// Last-seen timestamp per site in one query (grouped by site), for the status
// pills. Sites known to InfluxDB but silent for >24h simply won't appear here
// and are reported with lastSeen: null (offline).
async function allSitesLastSeen() {
  const rows = await flux(
    `from(bucket: "${INFLUX_BUCKET}")\n` +
      `  |> range(start: -24h)\n` +
      `  |> filter(fn: (r) => r._measurement == "telemetry")\n` +
      `  |> group(columns: ["site"])\n` +
      `  |> last()\n` +
      `  |> keep(columns: ["site", "_time"])`,
  );
  const out = {};
  for (const r of rows) {
    if (r.site && r._time) out[r.site] = r._time;
  }
  return out;
}

// Grafana lives on the cluster-internal network only. This proxies /grafana/*
// to it so the browser talks to this server's own origin — same trick as the
// Influx queries above, and it's what makes the charts work off the home LAN
// and avoids mixed-content (this server is the thing serving HTTPS).
const HOP_BY_HOP = new Set([
  'connection', 'keep-alive', 'transfer-encoding', 'upgrade', 'host', 'content-length',
]);

async function proxyGrafana(req, res) {
  const headers = Object.fromEntries(
    Object.entries(req.headers).filter(([k]) => !HOP_BY_HOP.has(k.toLowerCase())),
  );
  const hasBody = req.method !== 'GET' && req.method !== 'HEAD';
  const upstream = await fetch(`${GRAFANA_URL}${req.url}`, {
    method: req.method,
    headers,
    // /api/ds/query is a POST carrying the actual query in its body — without
    // forwarding it, Grafana gets an empty request and every panel shows
    // "No data". duplex: 'half' is required whenever body is a stream.
    body: hasBody ? req : undefined,
    duplex: hasBody ? 'half' : undefined,
    signal: AbortSignal.timeout(15000),
  });
  const resHeaders = Object.fromEntries(
    [...upstream.headers].filter(([k]) => !HOP_BY_HOP.has(k.toLowerCase()) && k.toLowerCase() !== 'content-encoding'),
  );
  res.writeHead(upstream.status, resHeaders);
  if (!upstream.body) return res.end();
  for await (const chunk of upstream.body) res.write(chunk);
  res.end();
}

function send(res, code, body, type) {
  res.writeHead(code, { 'Content-Type': type, 'Content-Length': Buffer.byteLength(body) });
  res.end(body);
}

const json = (res, code, payload) => send(res, code, JSON.stringify(payload), 'application/json');

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://localhost');
  try {
    if (url.pathname === '/' || url.pathname === '/index.html') {
      send(res, 200, INDEX, 'text/html; charset=utf-8');
    } else if (url.pathname === '/healthz') {
      send(res, 200, 'ok', 'text/plain');
    } else if (url.pathname.startsWith('/grafana/')) {
      await proxyGrafana(req, res);
    } else if (url.pathname === '/api/status') {
      const [known, lastSeen] = await Promise.all([listSites(), allSitesLastSeen()]);
      const statuses = known.map((s) => ({ site: s, lastSeen: lastSeen[s] ?? null }));
      json(res, 200, { statuses });
    } else if (url.pathname === '/api/summary') {
      const site = url.searchParams.get('site') || '';
      const rangeKey = url.searchParams.get('range') || '24h';
      const rangeFlux = RANGES[rangeKey];
      if (!rangeFlux) return json(res, 400, { error: 'unknown range' });
      const known = await listSites();
      if (!SITE_RE.test(site) || !known.includes(site)) {
        return json(res, 400, { error: 'unknown site' });
      }
      const [averages, extremes] = await Promise.all([
        siteAverages(site, rangeFlux),
        siteExtremes(site, rangeFlux),
      ]);
      json(res, 200, { site, range: rangeKey, averages, extremes });
    } else if (url.pathname.startsWith('/api/admin/')) {
      // POST only: these delete data, and a GET would be reachable from a link
      // or a prefetch.
      if (req.method !== 'POST') return json(res, 405, { error: 'method not allowed' });
      if (!ADMIN_PASSWORD) return json(res, 503, { error: 'admin disabled on this server' });
      if (!checkAdmin(req)) {
        const n = (authFails.get(peer(req)) || 0) + 1;
        authFails.set(peer(req), n);
        await sleep(Math.min(n * 500, 5000));
        res.setHeader('WWW-Authenticate', 'Basic realm="dashboard admin"');
        return json(res, 401, { error: 'unauthorized' });
      }
      authFails.delete(peer(req));

      const site = url.searchParams.get('site') || '';
      const known = await listSites();
      if (!SITE_RE.test(site) || !known.includes(site)) {
        return json(res, 400, { error: 'unknown site' });
      }

      if (url.pathname === '/api/admin/remove') {
        // Everything, for all time — the site stops existing once its last
        // point is gone, since a site is only ever a tag on its data.
        const before = await sitePointCount(site);
        await influxDelete(site, EPOCH, new Date().toISOString());
        const remaining = await sitePointCount(site);
        console.warn(
          `admin: remove ${JSON.stringify(site)} — ${before} points before, ${remaining} after`,
        );
        return json(res, 200, { ok: true, action: 'remove', site, before, remaining });
      }

      if (url.pathname === '/api/admin/reset') {
        const rangeKey = url.searchParams.get('range') || '';
        const ms = RANGE_MS[rangeKey];
        if (!ms) return json(res, 400, { error: 'unknown range' });
        const keptSince = new Date(Date.now() - ms).toISOString();
        const before = await sitePointCount(site);
        await influxDelete(site, EPOCH, keptSince);
        const remaining = await sitePointCount(site);
        console.warn(
          `admin: reset ${JSON.stringify(site)} kept since ${keptSince} — ` +
            `${before} points before, ${remaining} after`,
        );
        return json(res, 200, { ok: true, action: 'reset', site, keptSince, before, remaining });
      }

      json(res, 404, { error: 'not found' });
    } else {
      json(res, 404, { error: 'not found' });
    }
  } catch (e) {
    console.error(e.message);
    json(res, 502, { error: e.message });
  }
});

// Grafana Live (the dashboard's realtime channel, e.g. /grafana/api/live/ws)
// negotiates over a WebSocket, which is a raw TCP upgrade rather than a normal
// request/response — fetch() can't proxy it, so pipe the socket directly.
server.on('upgrade', (req, clientSocket, head) => {
  if (!req.url.startsWith('/grafana/')) return clientSocket.destroy();
  const { hostname, port } = new URL(GRAFANA_URL);
  const upstream = net.connect(Number(port) || 80, hostname, () => {
    const headerLines = Object.entries(req.headers).map(([k, v]) => `${k}: ${v}`);
    upstream.write(`${req.method} ${req.url} HTTP/1.1\r\n${headerLines.join('\r\n')}\r\n\r\n`);
    upstream.write(head);
    clientSocket.pipe(upstream);
    upstream.pipe(clientSocket);
  });
  upstream.on('error', () => clientSocket.destroy());
  clientSocket.on('error', () => upstream.destroy());
});

server.listen(PORT, () => console.log(`dashboard listening on :${PORT}`));
