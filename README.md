# solarman_bridge

Monitors a Deye three-phase hybrid inverter (SG04LP3 family) through its
Solarman / IGEN WiFi data logger and serves a web dashboard.

- Native Solarman V5 client (TCP 8899) carrying Modbus RTU, modelled on
  [pysolarmanv5](https://github.com/jmccrohan/pysolarmanv5): one persistent
  connection, unsolicited counter frames skipped, double-CRC quirk handled.
- Polls (3 register reads, ~0.85 s per cycle).
- SQLite history: raw samples (30 days by default) and one row per day with
  the inverter's own daily energy counters, keyed by the inverter clock.
- Dashboard: Realtime, History, Energy, Info.

## Configuration

`/etc/solarman_bridge/solarman_bridge.conf`. One `inverter "name" { ... }`
section per inverter, each with its own logger; `model`, `host` and
`serial` are required. The serial is the **logger's** serial number (the
logger's web page, `status.html`, shows it as the device serial number),
not the inverter's.

Every inverter gets its own poller and connection. They share one history
database, where the section title tells them apart: renaming a section
starts a new history.

## Inverter models

`model` picks the register map; the only one so far is
`deye_sg04lp3`. A model is a single file in `src/models/`: it defines an
`InverterModel` (the blocks to read, the decoders into the common
`InverterSample`, the run states, the commands) and registers it with a
static `ModelRegistrar`. CMake picks the file up by itself and the tests
check every registered model, so nothing else needs editing. Fields a
model does not have stay 0.

## HTTP API

| Path | Returns |
|---|---|
| `api/inverters` | the configured inverters |
| `api/live?inverter=` | latest sample, `online` / `connected`, sample age, inverter clock |
| `api/info?inverter=` | logger and inverter identity, poll statistics, last error |
| `api/history?inverter=&time_start=&time_end=&count=` | samples oldest first (unix seconds; decimated to `count` buckets, max 2000) |
| `api/daily?inverter=&year=&month=` | daily energy rows for one month; `inverter=all` sums all |
| `POST api/command` | queues a named command, body `{"inverter": "main", "name": "power_on"}`; the outcome shows up in `api/live` as `last_command` |

`inverter` is a section title; without it the first inverter is used.

Sample fields are integers whose names carry the unit: `_w`, `_mv`, `_ma`,
`_wh`, `_dc` (0.1 °C), `_chz` (0.01 Hz). Battery power is positive when
discharging; grid power is positive when importing.

Commands are off unless `control { enabled = true }` is set, and then each
one must carry the password whose crypt(3) hash is in
`control { password_hash }` (`openssl passwd -6`); five wrong passwords in
a row lock commands out for a minute. There is no raw register write: each
command owns some bits of one register, which is read, changed, written
with Modbus function 0x10 and read back. The power switch is register 80
bit 0. Off stops inverting and charging; the display stays alive. Every
write is logged. The endpoint takes only a JSON body, so a foreign web page
cannot trigger it without a CORS preflight.

## Behind a reverse proxy

With `http { bind = "127.0.0.1" }` the service is reachable only through a
reverse proxy on the same host; the pages use relative URLs, so it can
mount the service under any path prefix.

## Build, test, package

```sh
cmake -S . -B build && cmake --build build -j && (cd build && ctest)
dpkg-buildpackage -b -us -uc      # ../solarman-bridge_<version>_amd64.deb
```

Install on the target with `apt-get install ./solarman-bridge_<version>_amd64.deb`.

Run by hand: `SOLARMAN_BRIDGE_CONFIG=./my.conf build/solarman_bridge -D5`
(`-D5` also dumps every V5 frame).
