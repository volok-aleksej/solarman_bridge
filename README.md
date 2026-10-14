# solarman_bridge

Monitors a Deye three-phase hybrid inverter (SG04LP3 family) through its
Solarman / IGEN WiFi data logger and serves a web dashboard.

- Native Solarman V5 client (TCP 8899) carrying Modbus RTU, modelled on
  [pysolarmanv5](https://github.com/jmccrohan/pysolarmanv5): one persistent
  connection, unsolicited counter frames skipped, double-CRC quirk handled.
- Polls (4 register reads, ~0.7 s per cycle).
- SQLite history: raw samples (30 days by default) and one row per day with
  the inverter's own daily energy counters, keyed by the inverter clock.
- Dashboard: Realtime, History, Energy, Info.

## Configuration

`/etc/solarman_bridge/solarman_bridge.conf`; the required fields are
`logger.host` and `logger.serial`. The serial is the **logger's** serial
number (the logger's web page, `status.html`, shows it as the device serial
number), not the inverter's.

## HTTP API

| Path | Returns |
|---|---|
| `api/live` | latest sample, `online` / `connected`, sample age, inverter clock |
| `api/info` | logger and inverter identity, poll statistics, last error |
| `api/history?time_start=&time_end=&count=` | samples oldest first (unix seconds; decimated to `count` buckets, max 2000) |
| `api/daily?year=&month=` | daily energy rows for one month |

Sample fields are integers whose names carry the unit: `_w`, `_mv`, `_ma`,
`_wh`, `_dc` (0.1 °C), `_chz` (0.01 Hz). Battery power is positive when
discharging; grid power is positive when importing.

The pages use only relative URLs, so a reverse proxy can mount the service
under a path prefix (for example `/inverter/`).

## Build, test, package

```sh
cmake -S . -B build && cmake --build build -j && (cd build && ctest)
dpkg-buildpackage -b -us -uc      # ../solarman-bridge_<version>_amd64.deb
```

Install on the target with `apt-get install ./solarman-bridge_<version>_amd64.deb`.

Run by hand: `SOLARMAN_BRIDGE_CONFIG=./my.conf build/solarman_bridge -D5`
(`-D5` also dumps every V5 frame).
