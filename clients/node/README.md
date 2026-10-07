# macos-fan-control-client

Typed Node client for the [`fan_control`](https://github.com/triuzzi/macos-fan-control) CLI —
read and force **Apple Silicon** fan speed through the SMC.

It is a thin, dependency-free binding over the core's documented CLI + JSON contract
([CONTRACT.md](https://github.com/triuzzi/macos-fan-control/blob/main/CONTRACT.md)). It knows
no SMC keys: it runs the core and returns typed results.

## Install

```sh
npm install macos-fan-control-client
```

The [`fan_control`](https://github.com/triuzzi/macos-fan-control) binary must be installed
first (it is what actually talks to the SMC).

## Usage

```ts
import { readStatus, setPercent, setAutomatic, rpm, mode, average } from "macos-fan-control-client";

const status = await readStatus(true); // true = also read CPU/GPU temperatures
// { fanCount, fans: [{ index, actual, minimum, maximum, target, percent, forced }], temperatures }

await setPercent(70);  // force every fan to 70% of its own range (needs the scoped sudo rule)
await setAutomatic();  // hand every fan back to SMC firmware control
```

Reads (`readStatus`) need no privileges. Writes (`setPercent`, `setMaximum`, `setAutomatic`,
`runGuard`) run the core under `sudo -n` and require the one scoped NOPASSWD rule that
`scripts/install.sh` installs.

The binary path is `/usr/local/bin/fan_control` by default; override it with the
`FAN_CONTROL_BINARY` environment variable.

## API

| Export | Description |
|---|---|
| `readStatus(temps?)` | Full `FanStatus`; set `temps` to include CPU/GPU temperatures. |
| `setPercent(percent)` | Force every fan to a percentage of its own range. |
| `setMaximum()` | Force every fan to its maximum. |
| `setAutomatic()` | Return every fan to firmware control. |
| `runGuard(ceilingCelsius?)` | Revert to automatic if a sensor group exceeds the ceiling. |
| `rpm(value)` | Format RPM for display. |
| `mode(fans)` | `"Auto"`, `"Forced"` or `"Mixed"`. |
| `average(fans)` | Mean load percentage across fans. |
| `estimate(fan, percent)` | Preview the target RPM a percentage maps to. |

Failures throw `FanControlError` with the core's message (e.g. binary missing, or the sudo
rule not installed).

## License

MIT
