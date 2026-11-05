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
| 80 | 0x0050 | Remote on / off | 1 = on, 0 = off | R/W | **verified** — `0` → F19, `1` clears it. The Deye app uses this too. Not written by `solarman_bridge`: the on/off command was removed as unneeded and dangerous. |
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

Grid power is net: + when importing, − when exporting.

| Dec | Hex | Register | Unit / scale | Access | Notes |
|---|---|---|---|---|---|
| 598–600 | 0x0256 | Grid voltage L1 / L2 / L3 | 0.1 V | R | one register per phase (verified) |
| 604–606 | 0x025C | Internal CT power L1 / L2 / L3 | W, signed | R | equals grid power 622–624 on this unit (verified) |
| 609 | 0x0261 | Grid frequency | 0.01 Hz | R | verified |
| 610–612 | 0x0262 | Internal CT current L1 / L2 / L3 | 0.01 A, signed | R | inverter AC-port current, tracks the load — **not** net grid current (verified) |
| 613–615 | 0x0265 | External CT current L1 / L2 / L3 | 0.01 A, signed | R | grid-entry CT; reads ~0 here, no external CT installed (verified) |
| 622–624 | 0x026E | Grid power L1 / L2 / L3 | W, signed | R | net; + import / − export; = internal CT active power (verified) |
| 625 | 0x0271 | Grid power total | W, signed | R | verified |

There is **no per-phase grid-meter current register**: the inverter reports net
grid power directly, and the current registers are apparent CT currents at
different points (internal CT = inverter port, external CT = grid entry), so
`V × I` of any of them does not equal the net grid power.

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
limits, time-of-use windows, grid and generator rules.

The rows below are what the Settings tab shows and writes
(`setting_groups` in `src/models/deye_sg04lp3.cpp`; read as two blocks,
20–21 and 98–177).
**read** means: the address is in the ha-solarman `deye_p3` map and the value
read from this inverter on 2026-11-03 is the one configured on it (capacity
314 Ah as the BMS reports, time-of-use power 6000 W and SOC 50 % as set by
hand in all six windows, and so on). None of them has been pinned down by a
before/after write test yet.

| Dec | Hex | Register | Unit / scale | Access | Notes |
|---|---|---|---|---|---|
| 20–21 | 0x0014 | Rated power | 0.1 W, u32 | R | **verified** — 150000 on the 15 kW unit; read with the settings, the upper limit of the time-of-use power |
| 98 | 0x0062 | Battery type | 0 lead-acid · 1 lithium | R/W | read: 1; shown read-only |
| 102 | 0x0066 | Battery capacity | Ah | R/W | read: 314 |
| 108 | 0x006C | Max charge current | A | R/W | read: 90 |
| 109 | 0x006D | Max discharge current | A | R/W | read: 150 |
| 111 | 0x006F | Battery managed by | 0 voltage · 1 SOC · 2 no battery | R/W | read: 1; picks the SOC or the voltage settings; shown read-only |
| 115 | 0x0073 | Shutdown SOC | % | R/W | read: 10 |
| 116 | 0x0074 | Restart SOC | % | R/W | read: 20 |
| 117 | 0x0075 | Low battery SOC | % | R/W | read: 15 |
| 118 | 0x0076 | Shutdown voltage | 0.01 V | R/W | read: 4600 |
| 119 | 0x0077 | Restart voltage | 0.01 V | R/W | read: 5200 |
| 120 | 0x0078 | Low battery voltage | 0.01 V | R/W | read: 4750 |
| 121 | 0x0079 | Generator max run time | 0.1 h | R/W | read: 240 |
| 122 | 0x007A | Generator down time | 0.1 h | R/W | read: 0 |
| 123 | 0x007B | Generator charge start voltage | 0.01 V | R/W | read: 4900 |
| 124 | 0x007C | Generator charge start SOC | % | R/W | read: 15 |
| 125 | 0x007D | Generator charge current | A | R/W | read: 50 |
| 126 | 0x007E | Grid charge start voltage | 0.01 V | R/W | read: 4900 |
| 127 | 0x007F | Grid charge start SOC | % | R/W | read: 20 |
| 128 | 0x0080 | Grid charge current | A | R/W | read: 90 |
| 129 | 0x0081 | Generator charge | bit 0 | R/W | read: 1; ha-solarman also has a "battery wake up" value 0x101 here, so only bit 0 is written |
| 130 | 0x0082 | Grid charge | bit 0 | R/W | read: 1 |
| 140 | 0x008C | Charging signal | bit 0 grid · bit 1 generator | R/W | read: 3 |
| 142 | 0x008E | Work / energy-pattern mode | code | R/W | community — critical; not in the Settings tab |
| 146 | 0x0092 | Time of use | bit 0 on · bits 1–7 Mon–Sun | R/W | read: 255; not in the Settings tab |
| 148–153 | 0x0094 | Time of use: window start | hh × 100 + mm | R/W | read: 100 … 2100; not in the Settings tab |
| 154–159 | 0x009A | Time of use: power | W | R/W | read: 6000 × 6; one setting, written to all six |
| 160–165 | 0x00A0 | Time of use: voltage | 0.01 V | R/W | read: 4900 × 6; used when managed by voltage; one setting for all six |
| 166–171 | 0x00A6 | Time of use: SOC | % | R/W | read: 50 × 6; one setting for all six |
| 172–177 | 0x00AC | Time of use: charge sources | bit 0 grid · bit 1 generator | R/W | read: 3 × 6; one setting per bit for all six |

To pin any of them down, use the before/after snapshot method: read all
registers, change one value in the Deye app, read again, diff. The stable
settings-region register that changed is the one the app wrote (filter out live
telemetry in 500+ and the clock at 62–64).

## Sources

Verified rows: read from this inverter (SN 2512233176) over local Modbus and
confirmed in `src/models/deye_sg04lp3.cpp`. Community rows and the F19/F22
notes: the Deye SG04LP3-EU manual, [deye-controller](https://github.com/githubDante/deye-controller),
[ha-solarman](https://github.com/davidrapan/ha-solarman) and
[Marklabs](https://marklabs.pl/en/deye-modbus-complete-register-list/). Deye
publishes no single official Modbus map, so treat unverified addresses as
leads, not gospel — confirm by read before writing.
