# Deye SG04LP3 Modbus register reference

Holding registers of the Deye three-phase low-voltage hybrid (device type 5,
SG04LP3 family), read through the Solarman logger. Read with function `0x03`,
write with `0x10`.

Conventions on this inverter:

- 32-bit counters span two registers, **low word first**.
- Temperatures are `raw - 1000` in 0.1 °C (i.e. a +100.0 °C offset).
- Signed values are two's complement.
- Battery power is positive when discharging; grid power is positive when
  importing.

Confidence:

- **verified** — read from this inverter and confirmed in `solarman_bridge`
  (`src/models/deye_sg04lp3.cpp`), or pinned down by a before/after write test.
- **community** — from community maps (deye-controller, ha-solarman, Marklabs);
  not verified here. Treat as a lead; confirm by read before writing.

> **On/off is register 80, and it is not safe.** Writing `0` to reg 80 turns
> the inverter off and raises fault **F19**; writing `1` turns it on and clears
> F19. The official Deye app does the same over the cloud — there is no separate
> clean off register on this model. The documented safe remote shutdown is the
> hardware **RSD** terminals (fault **F22**), not Modbus.

## Identity & clock

| Dec | Hex | Register | Unit / scale | Access | Notes |
|---|---|---|---|---|---|
| 0 | 0x0000 | Device type | code | R | 5 = SG04LP3 three-phase LV hybrid (verified) |
| 3–7 | 0x0003 | Serial number | 10 ASCII | R | high byte first, 2 chars per register (verified) |
| 62 | 0x003E | Clock — year / month | hi: year+2000, lo: month | R/W | inverter RTC (verified) |
| 63 | 0x003F | Clock — day / hour | hi: day, lo: hour | R/W | verified |
| 64 | 0x0040 | Clock — minute / second | hi: minute, lo: second | R/W | verified |

## Control

| Dec | Hex | Register | Values | Access | Notes |
|---|---|---|---|---|---|
| 80 | 0x0050 | Remote on / off | 1 = on, 0 = off | R/W | **verified** — `0` → F19, `1` clears it. The Deye app uses this too. Do not use for off. |
| 93 | 0x005D | Forced generator | 0x11 = on, 0 = off | R/W | **verified** — app wrote `17 (0x11)` for on, `0` for off |

## State & alarms

| Dec | Hex | Register | Unit / scale | Access | Notes |
|---|---|---|---|---|---|
| 500 | 0x01F4 | Run state | code | R | 0 standby · 1 self-check · 2 normal · 3 alarm · 4 fault (verified) |
| 540 | 0x021C | DC / heatsink temperature | 0.1 °C | R | raw − 1000 (verified) |
| 541 | 0x021D | AC temperature | 0.1 °C | R | raw − 1000 (verified) |
| 553–554 | 0x0229 | Warning bitmask | u32 | R | 0 = none, low word first (verified) |
| 555–558 | 0x022B | Fault bitmask | u64 | R | bit `n` = fault `F(n+1)`; e.g. bit 18 = F19 (verified) |

## Battery

As the inverter sees the battery.

| Dec | Hex | Register | Unit / scale | Access | Notes |
|---|---|---|---|---|---|
| 586 | 0x024A | Battery temperature | 0.1 °C | R | raw − 1000 (verified) |
| 587 | 0x024B | Battery voltage | 0.01 V | R | verified |
| 588 | 0x024C | Battery SOC | % | R | verified |
| 590 | 0x024E | Battery power | W, signed | R | + discharge / − charge (verified) |
| 591 | 0x024F | Battery current | 0.01 A, signed | R | verified |

## Grid

Power is + when importing, − when exporting. Current is the inverter's
internal CT.

| Dec | Hex | Register | Unit / scale | Access | Notes |
|---|---|---|---|---|---|
| 598–600 | 0x0256 | Grid voltage L1 / L2 / L3 | 0.1 V | R | one register per phase (verified) |
| 609 | 0x0261 | Grid frequency | 0.01 Hz | R | verified |
| 610–612 | 0x0262 | Grid current L1 / L2 / L3 | 0.01 A, signed | R | internal CT (verified) |
| 622–624 | 0x026E | Grid power L1 / L2 / L3 | W, signed | R | + import / − export (verified) |
| 625 | 0x0271 | Grid power total | W, signed | R | verified |

## Inverter output & load

| Dec | Hex | Register | Unit / scale | Access | Notes |
|---|---|---|---|---|---|
| 633–635 | 0x0279 | Inverter output power L1 / L2 / L3 | W, signed | R | verified |
| 636 | 0x027C | Inverter output power total | W, signed | R | verified |
| 644–646 | 0x0284 | Load voltage L1 / L2 / L3 | 0.1 V | R | verified |
| 650–652 | 0x028A | Load power L1 / L2 / L3 | W | R | no load-current register; current ≈ P/U (verified) |
| 653 | 0x028D | Load power total | W | R | verified |

## PV strings

| Dec | Hex | Register | Unit / scale | Access | Notes |
|---|---|---|---|---|---|
| 672 | 0x02A0 | PV1 power | W | R | verified |
| 673 | 0x02A1 | PV2 power | W | R | verified |
| 676 | 0x02A4 | PV1 voltage | 0.1 V | R | verified |
| 677 | 0x02A5 | PV1 current | 0.1 A | R | verified |
| 678 | 0x02A6 | PV2 voltage | 0.1 V | R | verified |
| 679 | 0x02A7 | PV2 current | 0.1 A | R | verified |

## Energy today

The inverter's own daily counters, reset at its midnight. All in 0.1 kWh
(raw × 0.1).

| Dec | Hex | Register | Unit / scale | Access | Notes |
|---|---|---|---|---|---|
| 514 | 0x0202 | Battery charge today | 0.1 kWh | R | verified |
| 515 | 0x0203 | Battery discharge today | 0.1 kWh | R | verified |
| 520 | 0x0208 | Grid bought today | 0.1 kWh | R | verified |
| 521 | 0x0209 | Grid sold today | 0.1 kWh | R | verified |
| 526 | 0x020E | Load today | 0.1 kWh | R | verified |
| 529 | 0x0211 | PV today | 0.1 kWh | R | verified |

## Energy total

Lifetime counters. Each is a 32-bit value across two registers, low word
first, in 0.1 kWh.

| Dec | Hex | Register | Unit / scale | Access | Notes |
|---|---|---|---|---|---|
| 516–517 | 0x0204 | Battery charge total | 0.1 kWh, u32 | R | verified |
| 518–519 | 0x0206 | Battery discharge total | 0.1 kWh, u32 | R | verified |
| 522–523 | 0x020A | Grid bought total | 0.1 kWh, u32 | R | verified |
| 524–525 | 0x020C | Grid sold total | 0.1 kWh, u32 | R | verified |
| 527–528 | 0x020F | Load total | 0.1 kWh, u32 | R | verified |
| 534–535 | 0x0216 | PV total | 0.1 kWh, u32 | R | verified |

## Settings region (≈ 0–250)

Writable configuration — work mode, SOC floors, charge/discharge current
limits, time-of-use windows, grid and generator rules. On this inverter only
`reg 80` and `reg 93` (above) were pinned down by test; the rows below come
from community maps and are **not verified here**.

To map any specific setting safely, use the before/after snapshot method: read
all registers, change one value in the Deye app, read again, diff. The stable
settings-region register that changed is the one the app wrote (filter out live
telemetry in 500+ and the clock at 62–64). See
`memory` note `deye-reg80-f19` for the method.

| Dec | Hex | Register | Unit / scale | Access | Source |
|---|---|---|---|---|---|
| 108 | 0x006C | Max charge current | A | R/W | community |
| 109 | 0x006D | Max discharge current | A | R/W | community |
| 142 | 0x008E | Work / energy-pattern mode | code | R/W | community — critical; test with no critical load |

## Sources

Verified rows: read from this inverter (SN 2512233176) over local Modbus and
confirmed in `src/models/deye_sg04lp3.cpp`. Community rows and the F19/F22
notes: the Deye SG04LP3-EU manual, [deye-controller](https://github.com/githubDante/deye-controller),
[ha-solarman](https://github.com/davidrapan/ha-solarman) and
[Marklabs](https://marklabs.pl/en/deye-modbus-complete-register-list/). Deye
publishes no single official Modbus map, so treat unverified addresses as
leads, not gospel — confirm by read before writing.
