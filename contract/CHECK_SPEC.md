# Contract check: shared specification

Every component (C++, Python, Dart) implements the same startup check. This file
is the single description of the semantics; `check_vectors.json` holds test
cases that every implementation must pass unchanged.

## Inputs

| Input | Default | Override (tests, special setups) |
| --- | --- | --- |
| bundle file | `/etc/raccoon-bundle.json` | env `RACCOON_BUNDLE_FILE` |
| device marker directory | `/etc/raccoon-image` | env `RACCOON_DEVICE_MARKER` |
| status directory | `/run/raccoon` | env `RACCOON_STATUS_DIR` |
| own contract hash | compiled in (`contract.json`) | none |
| own transport version | compiled in, `dev` if unknown | none |
| component name and version | given by the component | none |
| component check | none | an optional result of a check that belongs to the component itself (see *Component check*) |

`RACCOON_CONTRACT_CHECK=force` runs the check even when the device marker is
missing (used to test on a laptop).

## Bundle file

The bundle manifest as written by the image build, copied to the device:

```json
{
  "bundle": "2026.11.02",
  "contract_hash": "93ac36836810c9fe",
  "components": { "raccoon-transport": "v69", "raccoon-cli": "v173" }
}
```

The transport check only reads `bundle`, `contract_hash` and
`components["raccoon-transport"]`. `contract_hash` holds the expected hash, or
the literal `dev` to switch the comparison to dev mode. The other entries of
`components` are the component's own business (see *Component check*). Unknown
keys are ignored.

## Decision

1. The device marker directory does not exist and the check is not forced:
   result `skipped` (not a Raccoon device, laptop, simulation). Never blocks.
2. The bundle file does not exist: `bundle-fehler`, reason `file-missing`.
3. The file cannot be read, is not valid JSON, or is not a JSON object:
   `bundle-fehler`, reason `file-unreadable`.
4. `contract_hash` is absent or not a string: `bundle-fehler`, reason `field-missing`.
5. `contract_hash` is empty or only whitespace: `bundle-fehler`, reason `field-empty`.
6. `contract_hash` is neither `dev` nor the own hash: `mismatch`.
7. The own hash matches, or `contract_hash` is `dev`: the transport's own checks
   passed. If the component brought a **component check**, it is merged now:
   - a result that is not `ok` and not `dev-warnung` **refuses**, with the
     result name, reason and message the component gave;
   - otherwise `dev-warnung` when `contract_hash` is `dev` (nothing to compare
     against; the own hash is recorded and the aggregate warns when active
     components disagree with each other); the transport's message is used;
   - otherwise the component's `dev-warnung`, with its message;
   - otherwise `ok`.

The transport knows nothing about what a component check checks. Precedence:
file problems, then `contract_hash` problems, then the transport `mismatch`,
then the component's refusal. A transport failure always wins, and off a device
(step 1) the component check is ignored.

Comparison is exact (case-sensitive) after trimming surrounding whitespace.

`mismatch`, `bundle-fehler` and **every other result except `ok`, `dev-warnung` and `skipped`** (that is, the refusals that component checks define, such as `version-mismatch`) **refuse**: the component must not start. A
component that refuses logs the message where it normally logs and exits with
the reserved exit code **78**. systemd units set `RestartPreventExitStatus=78`
so a refusal does not become a restart loop.

## Messages

Identical text in every language (placeholders in braces; unknown values are
printed as `unknown`):

- ok: `contract ok: {own_hash}`
- mismatch: `transport contract mismatch: {component} {component_version} uses contract {own_hash} (transport {own_version}), bundle {bundle} expects {expected_hash} (transport {expected_version}); update or reflash so all components match`
- dev-warnung (contract): `transport contract check is in dev mode (bundle field is 'dev'): {component} uses contract {own_hash} (transport {own_version}); mismatches are not enforced`
- bundle-fehler: `transport contract check failed: {reason_text}: {bundle_file}`, with reason text
  `bundle file is missing` / `bundle file is unreadable` /
  `bundle file has no contract_hash field` / `bundle file has an empty contract_hash`
- a component's own result: the message the component gave, as it is
- skipped: `not running on a raccoon device; contract check skipped`

## Status file

After every check the component writes `{status_dir}/{component}.json`
atomically (write a temporary file in the same directory, then rename). A
component only ever writes its own file. Failing to write must never stop the
component; it logs a warning.

```json
{
  "component": "stm32-data-reader",
  "component_version": "v58",
  "own_hash": "93ac36836810c9fe",
  "own_transport_version": "v69",
  "expected_hash": "93ac36836810c9fe",
  "expected_transport_version": "v69",
  "component_check": null,
  "bundle": "2026.11.02",
  "result": "ok",
  "reason": null,
  "message": "contract ok: 93ac36836810c9fe",
  "timestamp": "2026-11-02T10:15:00Z",
  "pid": 1234,
  "pid_starttime": 98765,
  "persistent": true
}
```

`persistent` is true for long-running services (stm32-data-reader, Bot UI,
Pi server) and false for processes that come and go (raccoon-lib in a run).
`expected_hash` is `null` when the bundle could not be read. `component_check` is the component's own check as it handed it over (`name`, `result`, `reason`, `message`, `expected`), or `null`. `pid_starttime` is
field 22 of `/proc/<pid>/stat` (clock ticks since boot), or `null` where `/proc`
is not available.

## Staleness and aggregation (Pi server)

A status file is **active** when its `pid` is alive and its `pid_starttime`
still matches. Inactive files are reported as `active: false` and never
deleted, so a reader that refused and exited stays visible.

Off a Raccoon device (same rule as the check: no device marker directory and not
forced) the aggregate is simply `ok` with `on_device: false`; the Pi server must be
usable on a laptop for development and tests.

The aggregate answers three things: the expected hash and transport version
from the bundle file, whether dev mode is on, and the list of components with
result and activity. A run is refused when a component with a refusing result (`mismatch`,
`bundle-fehler`, or a refusal of its component check) is active, or is `persistent` (a service that refused and
exited stays blocking until it starts again and overwrites its file). A
stale file of a non-persistent component (raccoon-lib from an earlier run) is
ignored, otherwise a run could never start again after the cause was fixed.
The aggregate also lists `dev_components`: components whose component check has the result `dev-warnung` (also when the contract is in dev mode too) and that are active or persistent, so that CLI and UI can say which component checks are being skipped. In dev mode, `hashes_differ` is true when the active
components do not all report the same own hash.

## Component check

Some checks belong to the component and not to the transport. A component runs
such a check itself and hands the result to the startup check, which merges it
(step 7 of the decision) and records it in the status file as `component_check`.
The transport does not know what it checks: it only needs `name`, `result`
(`ok`, `dev-warnung`, or a name that refuses), `message`, and optionally
`reason` and `expected`.

The first such check is the **component version check**, specified in
`contract/COMPONENT_VERSION_CHECK.md` and implemented by each component itself.
