# macos-fan-control

Read and force **Apple Silicon fan speed** through the SMC. All logic lives in a C core;
Raycast is just one frontend.

![Apple Silicon](https://img.shields.io/badge/apple%20silicon-required-black)
![Raycast](https://img.shields.io/badge/raycast-extension-red)
![npm](https://img.shields.io/npm/v/macos-fan-control-client)
![License: MIT](https://img.shields.io/badge/license-MIT-green)

## Architecture

```
core/                C. Every SMC key, every fan rule. Speaks CLI + JSON.
  └─ fan_control     also the CLI — no separate binary
clients/node/        typed binding over the JSON contract. Published to npm. No UI.
interfaces/
  └─ raycast/        presentation only. Zero SMC knowledge.
```

The layers are decoupled by a documented CLI + JSON contract ([CONTRACT.md](CONTRACT.md)),
not by shared code. A frontend never touches an SMC key, never shells out to `sudo`, and
never parses human text — it calls the core and renders the result.

`clients/node` is published as [`macos-fan-control-client`](https://www.npmjs.com/package/macos-fan-control-client)
so any JS/TS frontend can depend on a versioned binding instead of vendoring one.

## Install

```bash
git clone https://github.com/triuzzi/macos-fan-control.git
cd macos-fan-control
sudo ./scripts/install.sh
```

`install.sh` builds the core, installs it root-owned in `/usr/local/bin`, and adds one
scoped sudoers rule so a frontend can change fan speed without a password prompt:

```
<you> ALL=(root) NOPASSWD: /usr/local/bin/fan_control
```

Safe only because the binary is `root:wheel 0755` in a root-only-writable directory, so no
user process can swap it and inherit root. The script verifies that before writing the rule,
and validates the rule with `visudo` first.

Remove with `sudo rm /etc/sudoers.d/macos-fan-control /usr/local/bin/fan_control`.

## Raycast

Install **Mac Fan Control** from the [Raycast Store](https://www.raycast.com/triuzzi/mac-fan-control),
or run the extension in development mode:

```bash
cd interfaces/raycast && npm install && npm run dev
```

| Command | Mode | Action |
|---|---|---|
| Fan Status | view | Live RPM, target, mode, CPU/GPU temperature |
| Set Fan Speed | view | Force every fan to 100/85/70/55/40/25/0% |
| Set Fans to Maximum | no-view | Force maximum — assign a hotkey |
| Set Fans to Automatic | no-view | Hand back to firmware — assign a hotkey |
| Fan Menu Bar | menu-bar | Current speed in the menu bar |

## CLI

Reading needs no privileges, writing does.

```bash
fan_control status --json --temps
sudo fan_control set 60
sudo fan_control max
sudo fan_control auto
sudo fan_control guard --max-temp 95
fan_control keys Tg
```

`guard` is the thermal safety net: if any fan is forced and CPU or GPU exceeds the ceiling,
it hands control back to the firmware. It lives in the core, so every frontend inherits it —
poll it from whatever timer the frontend already has.

## Node client

```bash
npm install macos-fan-control-client
```

```ts
import { readStatus, setPercent, setAutomatic } from "macos-fan-control-client";

const status = await readStatus(true); // fans + CPU/GPU temperatures
await setPercent(70);                  // force every fan to 70% of its range
await setAutomatic();                  // hand back to firmware
```

## Adding a frontend

Consume the contract. Nothing else.

```bash
fan_control status --json --temps   # read
sudo fan_control set 60             # write, exit 0 means verified
```

An Alfred workflow is a script filter over those two calls. A menu-bar app is the same plus a
timer. No code from `interfaces/raycast` is involved.

## How it works

Two SMC keys per fan: `F<n>md` (`0` firmware, `1` forced) and `F<n>Tg` (target RPM, honoured
only while forced). `F<n>Mn`/`F<n>Mx` bound the range.

Three things differ from Intel Macs and cost real debugging time:

- The mode key is lowercase `F0md`. The Intel spelling `F0Md` returns `0x84`
  (`kSMCKeyNotFound`), which reads exactly like a permissions error and is not one.
- Fan values are `flt`, not `fpe2`. Decoding with the Intel shift makes every number wrong.
- Writes are acknowledged before they are readable — a read within ~250 ms returns the
  previous target. The core settles and re-reads, so exit 0 means the fans actually changed.

The SMC honours a forced target even when its own thermal demand is higher, so low settings
really do reduce cooling. Presets below 50% ask for confirmation.

Forced mode is volatile: a reboot resets `F<n>md` to `0`.

## License

MIT
