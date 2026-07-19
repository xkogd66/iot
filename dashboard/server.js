// Telemetry dashboard: per-site max/min for each sensor over a chosen window.
// Queries InfluxDB server-side so the token never reaches the browser.
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');

const { INFLUX_URL, INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET } = process.env;
for (const [k, v] of Object.entries({ INFLUX_URL, INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET })) {
  if (!v) {
    console.error(`missing required env var ${k}`);
    process.exit(1);
  }
}

const PORT = process.env.PORT || 8080;
const FIELDS = ['temp', 'humidity', 'pressure'];
const SITE_RE = /^[A-Za-z0-9_-]+$/;
// Allowlist: the ?range token maps to a Flux relative start. Nothing else
// reaches the query, so range can't be injected.
const RANGES = { '6h': '-6h', '24h': '-24h', '7d': '-7d' };
const INDEX = fs.readFileSync(path.join(__dirname, 'public', 'index.html'));

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

async function listSites() {
  const rows = await flux(
    `import "influxdata/influxdb/schema"\n` +
      `schema.tagValues(bucket: "${INFLUX_BUCKET}", tag: "site")`,
  );
  return [...new Set(rows.map((r) => r._value).filter(Boolean))].sort();
}

// 1-hour mean of each field. `site` is validated against listSites() before
// reaching here, so it cannot break out of the string literal below.
async function siteAverages(site) {
  const fieldFilter = FIELDS.map((f) => `r._field == "${f}"`).join(' or ');
  const rows = await flux(
    `from(bucket: "${INFLUX_BUCKET}")\n` +
      `  |> range(start: -1h)\n` +
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
        siteAverages(site),
        siteExtremes(site, rangeFlux),
      ]);
      json(res, 200, { site, range: rangeKey, averages, extremes });
    } else {
      json(res, 404, { error: 'not found' });
    }
  } catch (e) {
    console.error(e.message);
    json(res, 502, { error: e.message });
  }
});

server.listen(PORT, () => console.log(`dashboard listening on :${PORT}`));
