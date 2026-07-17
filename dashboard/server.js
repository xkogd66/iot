// Telemetry dashboard: 1-hour averages per site.
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

async function siteAverages(site) {
  // `site` is validated against listSites() before reaching here, so it cannot
  // break out of the string literal below.
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

// Timestamp of the site's most recent point, or null if it hasn't reported in
// the last day. The dashboard turns this into an up/down indicator: a board
// that's been unplugged simply stops writing, so a stale newest point is "down".
async function siteLastSeen(site) {
  const rows = await flux(
    `from(bucket: "${INFLUX_BUCKET}")\n` +
      `  |> range(start: -24h)\n` +
      `  |> filter(fn: (r) => r._measurement == "telemetry")\n` +
      `  |> filter(fn: (r) => r.site == "${site}")\n` +
      `  |> group()\n` +
      `  |> last()`,
  );
  const times = rows.map((r) => r._time).filter(Boolean).sort();
  return times.at(-1) ?? null;
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
    } else if (url.pathname === '/api/sites') {
      json(res, 200, { sites: await listSites() });
    } else if (url.pathname === '/api/summary') {
      const site = url.searchParams.get('site') || '';
      const known = await listSites();
      if (!SITE_RE.test(site) || !known.includes(site)) {
        return json(res, 400, { error: 'unknown site' });
      }
      const [averages, lastSeen] = await Promise.all([siteAverages(site), siteLastSeen(site)]);
      json(res, 200, { site, window: '1h', averages, lastSeen });
    } else {
      json(res, 404, { error: 'not found' });
    }
  } catch (e) {
    console.error(e.message);
    json(res, 502, { error: e.message });
  }
});

server.listen(PORT, () => console.log(`dashboard listening on :${PORT}`));
