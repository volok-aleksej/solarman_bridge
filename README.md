# solarman_bridge

Monitors a Deye three-phase hybrid inverter (SG04LP3 family) through its
Solarman / IGEN WiFi data logger and serves a web dashboard.

- Native Solarman V5 client (TCP 8899) carrying Modbus RTU, modelled on
  [pysolarmanv5](https://github.com/jmccrohan/pysolarmanv5): one persistent
  connection, unsolicited counter frames skipped, double-CRC quirk handled.
- Polls (3 register reads, ~0.85 s per cycle).
- SQLite history: raw samples (30 days by default) and one row per day with
  the inverter's own daily energy counters, keyed by the inverter clock.
- Dashboard: Realtime, History, Energy, Settings, Info.

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
`InverterModel` and registers it with a static `ModelRegistrar`. The model
itself is only a list: the identity block, the blocks read on every cycle,
the blocks of the settings and the settings in their groups. What the
registers mean lives with them: each block has a decoder that puts its own
registers into the common `InverterSample`, `InverterClock` or
`InverterIdentity` (the run state arrives there with its name and the
"on" flag, ready to show), and each setting has its own functions for
whether the inverter has it, its limits and whether it fits with the
others. CMake picks the file up by itself and the tests
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
| `GET api/settings?inverter=` | every setting of the inverter, in groups: name, label, kind, unit, limits, value |
| `PUT api/settings?inverter=&<name>=<value>&...` | changes the named settings; no body; answers when the change is written and the settings are read again |

`inverter` is a section title; without it the first inverter is used.

Sample fields are integers whose names carry the unit: `_w`, `_mv`, `_ma`,
`_wh`, `_dc` (0.1 °C), `_chz` (0.01 Hz). Battery power is positive when
discharging; grid power is positive when importing.

The only thing the service writes to the inverter is a settings change
(below), and only with `control { enabled = true }` and the password whose
crypt(3) hash is in `control { password_hash }` (`openssl passwd -6`); five
wrong passwords in a row lock changes out for a minute. There is no raw
register write and no way to switch the inverter on or off.

## Settings

The Settings tab shows the inverter's own settings and changes them. What
there is to show comes from the model: `setting_groups` in its file lists
the groups (the page's tabs) and, for every setting, the registers and bits
that hold it, its kind, unit and limits, and when the inverter has it. A
setting with several registers is one value for all of them: a write sets
every register, a read takes the first. A limit is a fixed value or a function of the settings registers: the
time-of-use power goes up to the rated power, which is read along with the
settings like any other register. `GET api/settings` hands that description out together with the values;
the page draws whatever it gets and knows no setting by name. Every group
is a plain list of settings.

For `deye_sg04lp3` the groups are Time of use (power, SOC or voltage and
charge sources, each set for all six windows at once), Charge & signals
(grid and generator charging) and Battery limits. Which of the SOC and the voltage settings exist depends on
how the battery is managed (register 111); the other set is left out.

Every setting has its own name (`tou_power`).
`PUT api/settings?<name>=<value>&...` changes the ones it names, any number
in one request and nothing else: there is no body and no GET of a single
setting. Values are numbers in the setting's unit (`47.5`), `true` / `false`
(or `1` / `0`) for a flag, the index for a choice. A name that does not
exist in the current battery mode, a read-only setting and a value off its
range or step are refused with 400 and nothing is written; so is a change
after which the settings would not agree with each other (shutdown, low
battery and restart levels in order).

The request is answered when the change is over, a few seconds later:

| Code | Meaning |
|---|---|
| 200 `{"changed":true}` | every register is written and the settings are read again: `GET api/settings` shows what the inverter holds now |
| 200 `{"changed":false}` | the inverter held these values already, nothing is written |
| 400 | refused before anything is written, with the reason |
| 401, 429 | wrong password; too many of them |
| 502 | a write failed, or the settings could not be read after it; the reason names the registers. Other registers of the change may have been written |
| 503 | the logger is not connected, the settings are not read yet, or an earlier change is not over |

A change needs the control password, sent as `Authorization: Basic` (the
user name is ignored).

Nothing is read for a change. The service works out what to write from the
settings as it read them last (they are read every 15 s, and right after a
change), so a setting changed on the inverter's screen within those seconds
is not seen yet: a flag that shares its register with the one being changed
goes back to what was read. Only the registers that differ are written, each
run of neighbours with one `0x10` request, and only registers of settings
the model marks writable. Every write is logged with the old and the new
values. The next change is taken after the settings have been read again.

The regular read every 15 s also shows on the page the changes made on the
inverter's screen or in the vendor's app.

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
