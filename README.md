# MXChip Home Telemetry

Environmental telemetry from **MXChip AZ3166 IoT DevKit** boards deployed across two
locations (main home + summer home), aggregated into a single Grafana dashboard.

Repurposing a drawer full of AZ3166 boards left over from Microsoft-era projects into a
small, resilient home sensor network.

## Goal

Drop a couple of boards in each house and get nice time-series dashboards of indoor
climate — temperature, humidity, pressure — with the ability to compare rooms and
compare the two houses side by side.

## Hardware

**MXChip AZ3166 IoT DevKit** — one or more per site.

| Component | Part | Used for |
|-----------|------|----------|
| MCU | STM32F412RG (Cortex-M4F, 100 MHz) | firmware |
| Wi-Fi | EMW3166 (2.4 GHz b/g/n) | connectivity |
| Display | 128×64 OLED | local live readout |
| Temp / humidity | HTS221 | telemetry |
| Pressure | LPS22HB | telemetry |
| (unused for now) | LIS2MDL magnetometer, LSM6DSL accel/gyro, mic | future |

> The original Microsoft "Azure IoT Device Workbench" tooling is deprecated. Firmware is
> built with **PlatformIO** targeting `board = mxchip_az3166`.

## Architecture

```
[AZ3166 boards, both sites] --MQTT/TLS 8883--> [external nginx (stream)] --> [k3s: Mosquitto]
                                                                                    |
                                                                      [Telegraf] --> [InfluxDB]
                                                                                         |
[browser] --https--> [Cloudflare] --> [Grafana web UI] <--------------------------------+
```

The k3s cluster is the hub. Boards are dumb MQTT publishers. All aggregation, storage,
and visualization happen in-cluster.

### Data flow

1. Each board reads HTS221 + LPS22HB on an interval, shows current values on the OLED,
   and publishes a JSON payload over **native MQTT/TLS**.
2. An external **nginx `stream` (L4/TCP) proxy** ingests MQTT on 8883 and forwards to the
   Mosquitto `LoadBalancer` service on the cluster.
3. **Telegraf** (`mqtt_consumer` input) subscribes to the sensor topics and writes to
   **InfluxDB** — no hand-written bridge.
4. **Grafana** reads from InfluxDB. Its web UI is published through **Cloudflare** like
   the other services on the cluster.

## Key design decisions

- **MQTT → InfluxDB → Grafana**, self-hosted on the existing **k3s** cluster. Chosen over
  Home Assistant because the goal is polished time-series charts, and there's prior
  experience with Mosquitto + InfluxDB (manifests exist to re-provision).
- **Ingest via nginx `stream`, not Cloudflare.** Cloudflare's proxy is HTTP/HTTPS only;
  raw MQTT needs paid Spectrum, and the only free-tier alternative — MQTT over
  WebSockets — is painful on the AZ3166's Arduino MQTT stack. So boards speak **native
  MQTT/TLS** through nginx, and Cloudflare is reserved for HTTP services (Grafana UI).
- **TLS terminated at nginx** (boards trust the nginx cert; plaintext hop inside the
  trusted network). Simpler than end-to-end passthrough and fine for home telemetry.
- **Telegraf instead of a custom bridge** — declarative MQTT→InfluxDB, runs entirely
  in-cluster, no external exposure.

### Open decision: summer-home connectivity

How the summer-home boards reach the cluster, still to be decided:

- **A. Direct public TLS** — port-forward 8883 to nginx; per-device auth (user/pass or
  client certs). Least hardware, more exposure, no offline buffering.
- **B. Local bridge box over VPN** — a small always-on box (e.g. Pi Zero) at the summer
  home runs a local Mosquitto that **bridges** to the cluster broker over
  Tailscale/WireGuard. Safer, and buffers readings when the summer-home internet drops.
  Costs one cheap device per remote site.

Leaning **B** for the summer home given likely-flaky seasonal internet; the main home can
publish directly.

## Topic design

Site and room live in the topic path (Telegraf turns them into tags for Grafana template
variables). Payload is JSON so Telegraf can parse fields directly.

```
topic:   sensors/<site>/<room>
example: sensors/summerhouse/kitchen

payload: {"temp":21.4,"humidity":48.2,"pressure":1013.2,"rssi":-58}
```

## Repository layout (planned)

Standalone repo: **`github.com/xkogd66/iot`**.

```
iot/
├── README.md
├── firmware/    # PlatformIO project (sensor read + OLED + MQTT/TLS publish)
├── kustomize/   # k3s manifests: Mosquitto, InfluxDB, Telegraf, Grafana datasource
└── nginx/       # stream config for the MQTT ingest hop
```

> Reusable pieces (old Mosquitto/InfluxDB manifests, kustomize conventions) will be
> migrated over from the `hbvu_attic_lab` repo as needed.

## Status / next steps

- [ ] Firmware skeleton — PlatformIO, publish to a local broker first (fastest feedback)
- [ ] Re-provision Mosquitto + InfluxDB manifests (Mosquitto 2.x listener syntax +
      InfluxDB 2.x auth both changed since the old manifests)
- [ ] Telegraf manifest + Grafana InfluxDB datasource
- [ ] nginx `stream` ingest config + TLS certs
- [ ] Decide summer-home connectivity (A vs B) and provision
- [ ] Build Grafana dashboard (per-room + house-vs-house comparison)
