# Secrets

**No secrets live in this repo.** Kubernetes `Secret` manifests are kept out of
git and applied out-of-band. `.gitignore` blocks `secret.yaml` / `*.secret.yaml`
so a plaintext secret can't be committed by accident.

No extra tooling (Sealed Secrets, SOPS, Vault) — for a single-operator home
cluster, keeping secrets out of git is enough.

## How it works

- A plaintext `secret.yaml` lives locally (and/or in your own password manager),
  **not** in the repo.
- You apply it manually: `kubectl apply -f secret.yaml`.
- Deployments read the values via `secretKeyRef`, so the manifests in git never
  contain the secret — only the reference.

## Where the secrets live (storage & recovery)

- **Canonical copy — Proton Pass.** Each `secret.yaml` is stored in Proton Pass,
  one item per secret (name it `k8s/iot/<secret-name>`, e.g. `k8s/iot/influxdb-auth`):
  - **Secure Note** — paste the full `secret.yaml` into the note body. Works on
    any plan; the whole item is end-to-end encrypted.
  - **File attachment** (paid plans) — attach the `secret.yaml` directly if you'd
    rather keep it as a file.

  Encrypted, synced, and backed up by Proton — survives losing both the cluster
  and the laptop. This is the source of truth.
- **Live recovery from the cluster (fallback).** The currently-applied values are
  always readable from etcd:
  ```sh
  kubectl get secret influxdb-auth -n iot -o yaml                              # all keys, base64
  kubectl get secret influxdb-auth -n iot -o jsonpath='{.data.admin-token}' | base64 -d; echo
  ```
  Useful in a pinch, but not a backup — a dead cluster takes them with it.
- **Local working copy (optional).** A gitignored dir you apply from, e.g.
  `~/k8s-secrets/iot/`. Convenient but unencrypted at rest; the password manager
  stays the source of truth.

**Recovery drill:** to rebuild from scratch, pull each `secret.yaml` from the
password manager and `kubectl apply -f` it *before* applying the Deployments that
reference it.

## Secrets in this cluster (namespace `iot`)

| Secret         | Keys                                                        | Consumed by                          |
|----------------|-------------------------------------------------------------|--------------------------------------|
| `influxdb-auth`| admin-username, admin-password, admin-token, org, bucket    | InfluxDB, Telegraf, dashboard, Grafana |
| `grafana-auth` | admin-password                                              | Grafana (`GF_SECURITY_ADMIN_PASSWORD`) |

Keep a local `secret.yaml` for each (or `kubectl create secret generic ...`).
Example for `grafana-auth`:

```sh
kubectl create secret generic grafana-auth -n iot \
  --from-literal=admin-password='<password>'
```

## Cleaning up what's already committed

`influxdb/secret.yaml` was committed in plaintext. It has been untracked with
`git rm --cached` (the local copy stays on disk, now gitignored):

```sh
git rm --cached influxdb/secret.yaml   # done — stops tracking, keeps the file
```

Commit the removal to take it out of the working tree. The value still lives in
history until rotated — see below.

`grafana/secret.yaml` was never tracked, so it needs no cleanup.

## Rotation (the InfluxDB values are in git history)

`influxdb/secret.yaml` was committed in plaintext (commit `5bb3840`), so its
values — including the admin token `my-super-secret-admin-token` and password —
live in git history forever. Removing the file now does **not** scrub the past,
so these should be rotated:

- **InfluxDB admin token + password** — mint a new token in InfluxDB, update the
  local `influxdb-auth` secret, re-apply, then restart every consumer (Telegraf,
  dashboard, Grafana).

The **Grafana** admin password does *not* need rotation on these grounds: the
only value ever committed was the throwaway default `admin`. The real password
(`adminPWD4i0t`) exists only in the local, never-tracked `grafana/secret.yaml` —
keep it that way and don't commit it.
```
