# Contract

The boundary between `core/` and any frontend. Frontends depend on this, never on core internals.

## Invocation

```
fan_control status [--json] [--temps]
fan_control set <0-100>
fan_control max
fan_control auto
fan_control guard [--max-temp <celsius>]
fan_control keys [prefix]
```

Reads run unprivileged. `set`, `max`, `auto` and `guard` write SMC keys and require root.

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
  }
}
```

- All RPM fields are integers; `percent` is one decimal.
- `percent` is position within `minimum`–`maximum`, not a duty cycle.
- `forced` is `false` when the SMC firmware owns the fan.
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

## Stability

Field names and exit codes are the contract. Adding a field is backwards compatible;
renaming or removing one is not.
