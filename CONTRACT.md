# Contract

The boundary between `core/` and any frontend. Frontends depend on this, never on core internals.

## Invocation

```
fan_control status [--json] [--temps]
fan_control set <0-100>
fan_control max
fan_control auto
fan_control guard [--max-temp <celsius>]
fan_control cool [<0-100>] [--below <celsius>] [--max-temp <celsius>]
fan_control trigger [<0-100>] [--on <celsius>] [--below <celsius>] [--sensor max|cpu|gpu] [--max-temp <celsius>]
fan_control keys [prefix]
```

Reads run unprivileged. `set`, `max`, `auto`, `guard`, `cool` and `trigger` write SMC keys and
require root.

## Exit codes

| Code | Meaning |
|---|---|
| `0` | Success. For writes this means the change was **read back and verified**, not merely issued. |
| `1` | Runtime failure — SMC unreachable, key rejected, or the write did not take. |
| `2` | Bad usage or out-of-range argument. |

A write returning `0` is a guarantee the fans changed. SMC writes are acknowledged before
they become readable, so the core settles ~400 ms and re-reads before reporting success.

## `status --json`

```json
{
  "fanCount": 2,
  "fans": [
    {
      "index": 0,
      "actual": 5344,
      "minimum": 1350,
      "maximum": 5349,
      "target": 5349,
      "percent": 100.0,
      "forced": true
    }
  ],
  "temperatures": {
    "cpu": 51.3,
    "cpuSensors": 23,
    "gpu": 48.7,
    "gpuSensors": 84
  },
  "cooling": {
    "percent": 100,
    "below": 50,
    "pid": 4212
  },
  "trigger": {
    "percent": 85,
    "on": 80,
    "below": 60,
    "sensor": "max",
    "state": "armed",
    "pid": 4213
  }
}
```

- All RPM fields are integers; `percent` is one decimal.
- `percent` is position within `minimum`–`maximum`, not a duty cycle.
- `forced` is `false` when the SMC firmware owns the fan.
- `cooling` is present only while a background `cool` watcher is running, and reports the held
  speed, the release threshold and the watcher's pid. It is read from the same state a
  frontend can use to tell whether a cooldown is in progress.
- `trigger` is present only while a background `trigger` watcher is armed, and reports the
  trigger temperature, the held speed, the release threshold, the sensor group, the current
  `state` (`armed` or `holding`) and the watcher's pid. A frontend can read it to show whether
  the trigger is waiting or already spinning the fans.
- Only one watcher runs at a time, so `cooling` and `trigger` never appear together.
- `temperatures` is present only with `--temps`. It costs ~500 ms because it walks the whole
  SMC key index; a plain `status` is ~10 ms. Poll accordingly.
- `cpu` averages the `Tp*` sensor group, `gpu` averages `Tg*`. Other `T*` groups are not die
  sensors and are excluded.

## `set` / `max` / `auto`

`set <0-100>` maps a percentage onto each fan's own range:

```
target = minimum + (maximum - minimum) * percent / 100
```

That mapping is part of this contract, so a frontend may use it to preview a setting before
applying it. Changing it is a breaking change. The same percentage yields different RPM per
fan, because each fan has its own range. `max` is `set 100`. `auto` clears forced mode and
returns the fan to firmware control.

## `guard`

```
guard tripped at 97.4C (ceiling 95C)   # reverted to automatic
ok 51.3C                                # no action
```

Reverts every fan to firmware control if any fan is forced and either sensor group exceeds
the ceiling. A no-op when no fan is forced. Default ceiling is 95 °C. Intended to be polled
by whatever timer a frontend already has.

## `cool`

```
fan 0 forced to 5349 rpm (100%)
fan 1 forced to 5990 rpm (100%)
cooling at 100% until below 50C (background)
```

Forces every fan to `<0-100>` (default `100`, i.e. maximum), then returns **immediately**,
leaving a detached watcher in the background. The watcher re-reads the sensors once a second
and, when the hottest sensor group drops below `--below` (default 50 °C), hands the fans back
to the firmware and exits. It is a background cooling burst: `cool` itself exits `0` as soon
as the fans are verified forced and the watcher is running.

The watcher is **superseded by the next instruction**: running `set`, `max`, `auto`, `cool` or
another `trigger` stops the current watcher before applying the new setting, so the last
instruction wins. A `guard` trip also stops it. There is never more than one watcher.

The held speed and the threshold are both parameters, so `cool 100 --below 45` means "pin the
fans to maximum until the dies are under 45 °C". The requested speed is held for the whole
duration; unlike `guard`, it does not react to the temperature in between.

If the hottest group rises above `--max-temp` (default 95 °C) during the hold — possible when
the requested speed is below what the firmware would have chosen — or the sensors become
unreadable, the watcher hands the fans back to the firmware and stops.

Exit `0` means the fans were forced and the background watcher started; `1` means the fans
could not be forced or the watcher could not start; `2` is a bad argument.

## `trigger`

```
trigger armed at 80C: hold 85% until below 60C (max, background)
```

A thermostat with hysteresis, running as a detached watcher. It arms at `<--on>` (default
80 °C): when the watched sensor group reaches it, every fan is forced to `<0-100>` (default
`100`, i.e. maximum) and held there until the group drops to `--below` (default 60 °C), at
which point the fans go back to firmware control and the watcher goes back to waiting. It
stays armed, so the cycle repeats for as long as the watcher runs. `trigger` returns as soon
as the watcher is running; if the sensor is already at or above `--on` when it is invoked, the
fans are forced immediately and the watcher starts in the `holding` state.

`--sensor` picks which average the thresholds are compared against: `max` (default) is the
hottest of the CPU and GPU groups, `cpu` and `gpu` are that group alone. A group with no
readable sensors is refused rather than read as 0 °C, and if it becomes unreadable while the
watcher runs, the fans go back to the firmware and the watcher stops.

Arming in the `armed` state hands the fans back to the firmware: a trigger only forces when it
fires, so whatever a previous watcher (or a manual `set`) left forced is released. Arming in the
`holding` state forces immediately, as above.

`--on` must be above `--below` (the hysteresis band) and under `--max-temp` (default 95 °C).
If the watched group rises above the ceiling while holding — possible when the requested
speed is below what the firmware would have chosen — the watcher hands the fans back to the
firmware and stops instead of flapping. A release that the SMC rejects is retried on the next
tick, so the state never claims the fans were handed back when they were not.

Exit `0` means the watcher started **and its state was recorded**; `1` means the fans could not
be forced, the watched group has no sensors, or the watcher could not start — in every one of
those the fans are handed back to the firmware before exiting; `2` is a bad argument. Numbers
are parsed strictly: `trigger abc` and `--on 80abc` are exit `2`, never a coerced value.

The watcher is superseded exactly like `cool`: `set`, `max`, `auto`, `cool` or another
`trigger` stops it, and so does a `guard` trip.

## Stability

Field names and exit codes are the contract. Adding a field is backwards compatible;
renaming or removing one is not.
