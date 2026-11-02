# Component version check

Not part of raccoon-transport's code: **each component implements it itself** (raccoon-lib and
the Pi server in Python, the STM reader in C++, the Bot UI in Dart) and hands the result to the
startup check as a *component check* (see `CHECK_SPEC.md`). This file only says what they all do,
so that they behave the same.

## What it checks

The component's own version against its entry in the bundle file's `components` map
(`/etc/raccoon-bundle.json`, same path/env as the transport check).

| Component | Own version | Entry in `components` |
|-----------|-------------|-----------------------|
| `raccoon-lib` | version of `raccoon-library` / `raccoon-sim` | `raccoon-lib` |
| Pi server `raccoon-server` | `raccoon_cli.__version__` | `raccoon-cli` |
| `stm32-data-reader` | `STMREADER_VERSION` | `stm32-data-reader` |
| Bot UI `botui` | contents of `/home/pi/stp-velox/version` | `botui` |

## Decision

Read the entry `components[key]` of the bundle file. If the file cannot be read, return no component
check at all (the transport check reports that). Then:

1. absent or not a string: `bundle-fehler`, reason `version-field-missing`;
2. empty or only whitespace: `bundle-fehler`, reason `version-field-empty`;
3. the literal `dev`: `dev-warnung`, reason `version-omitted` (the comparison is omitted);
4. otherwise compare after normalising (trim, drop one leading `v` or `V` on both sides, nothing
   else): different is `version-mismatch`, equal is `ok`.

The component check is independent of the contract: a `dev` contract does not excuse a wrong
version, and a `dev` entry does not excuse a wrong contract.

## Results and messages

`name` is `version`; `expected` is the trimmed entry.

- `version-mismatch`: `component version mismatch: {component} is {component_version} but bundle {bundle} expects {expected}; update or reflash so all components match` (`{bundle}` is `unknown` when the file has no `bundle`)
- `dev-warnung` / `version-omitted`: `component version check omitted ('{key}' is 'dev' in the bundle): {component} {component_version} is not compared; mismatches are not enforced`
- `bundle-fehler` / `version-field-missing`: `bundle check failed: bundle file has no version for {key}: {bundle_file}`
- `bundle-fehler` / `version-field-empty`: `bundle check failed: bundle file has an empty version for {key}: {bundle_file}`

## Cases every implementation must pass

Own version `v58`, key `stm32-data-reader`, bundle name `b`:

| `components["stm32-data-reader"]` | Result | Reason |
|---|---|---|
| `"v58"` | `ok` | |
| `"58"` | `ok` | |
| `" v58 "` | `ok` | |
| `"V58"` | `ok` | |
| `"v59"` | `version-mismatch` | |
| `"v058"` | `version-mismatch` | |
| `"dev"` | `dev-warnung` | `version-omitted` |
| `""` | `bundle-fehler` | `version-field-empty` |
| `"  "` | `bundle-fehler` | `version-field-empty` |
| missing key | `bundle-fehler` | `version-field-missing` |
| `58` (number) | `bundle-fehler` | `version-field-missing` |
| no `components` | `bundle-fehler` | `version-field-missing` |
