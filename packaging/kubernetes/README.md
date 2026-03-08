# Kubernetes packaging for cacti-spine

Spine is a short-lived one-shot SNMP poller. It runs, polls all devices, writes
results to MySQL, then exits. The natural Kubernetes primitive is a **CronJob**,
not a Deployment. One CronJob fires per polling interval (default: every minute).

Two packaging options are provided:

- `helm/` — parameterised Helm v3 chart, suitable for most deployments.
- `kustomize/base/` — plain YAML base for kustomize overlays.

## Prerequisites

- kubectl 1.25+
- Helm v3 (for the Helm path)
- A Kubernetes cluster running PodSecurity **Baseline** or **Privileged** admission
  (see NET_RAW note below)
- A MySQL/MariaDB instance reachable from the cluster

## Helm

### Install

```sh
helm install spine ./packaging/kubernetes/helm \
  --set db.host=mydb.internal \
  --set db.password=secret
```

All configurable fields are in `helm/values.yaml`. Notable overrides:

| Flag | Default | Notes |
|------|---------|-------|
| `db.host` | `localhost` | MySQL hostname |
| `db.password` | `cactiuser` | Use `db.existingSecret` in production |
| `db.existingSecret` | `""` | Name of a pre-existing Secret; skips rendering `secret.yaml` |
| `schedule` | `*/1 * * * *` | CronJob schedule (every minute) |
| `pollerCount` | `1` | Number of CronJobs (see distributed polling below) |
| `netRawCapability` | `true` | Set `false` to drop NET_RAW when ICMP ping is not used |
| `image.tag` | `1.3.0` | Override to pin a specific spine release |

### Upgrade

```sh
helm upgrade spine ./packaging/kubernetes/helm --reuse-values \
  --set image.tag=1.4.0
```

### Uninstall

```sh
helm uninstall spine
```

## Kustomize

The base provides working defaults with placeholder credentials. Create an
overlay for each environment.

```sh
# Apply the base directly (development / quick test only — change the password first)
kubectl apply -k packaging/kubernetes/kustomize/base/

# Recommended: create an overlay
mkdir -p packaging/kubernetes/kustomize/overlays/production
```

A minimal overlay `kustomization.yaml`:

```yaml
apiVersion: kustomize.config.k8s.io/v1beta1
kind: Kustomization
bases:
  - ../../base
patches:
  - path: patch-db.yaml
secretGenerator:
  - name: cacti-spine
    literals:
      - db-password=<real-password>
    options:
      disableNameSuffixHash: true
```

## NET_RAW capability

Spine uses raw sockets for ICMP ping checks. In Kubernetes this requires
`securityContext.capabilities.add: ["NET_RAW"]`.

- **PodSecurity Baseline** (default for most clusters): NET_RAW is allowed.
- **PodSecurity Restricted**: NET_RAW is blocked. Either relax the namespace
  policy or set `netRawCapability: false` and configure spine to use SNMP-only
  polling (no ping checks).

The manifests also drop all other capabilities (`drop: ["ALL"]`) and set
`readOnlyRootFilesystem: true`, so the attack surface outside NET_RAW is
minimal.

## Distributed polling

Cacti supports multiple Data Collectors, each identified by a `--poller <id>`
argument. To run N pollers in parallel, each responsible for a subset of
devices, set `pollerCount`:

```sh
helm install spine ./packaging/kubernetes/helm \
  --set pollerCount=3 \
  --set db.host=mydb.internal \
  --set db.password=secret
```

This renders three CronJobs: `spine-cacti-spine-0`, `spine-cacti-spine-1`,
`spine-cacti-spine-2`, invoking spine with `--poller 0`, `--poller 1`, and
`--poller 2` respectively. Each CronJob fires independently on the same
schedule with `concurrencyPolicy: Forbid`.

For the kustomize path, copy `base/cronjob.yaml` once per poller, change the
`--poller` argument, and reference all copies from your overlay
`kustomization.yaml`.

## Container image

The chart defaults to `ghcr.io/cacti/spine:1.3.0`, built and pushed by the
spine release workflow in `.github/workflows/`. Override `image.repository` and
`image.tag` to use a private registry or a different version.
