# MeshAddon protocols (v0.2 draft)

**Principle:** the Heltec answers every status and sensor command itself. The Pi link is used **only** to list and run scripts.

There are three interfaces:

```
 Browser (Web Serial) ──USB CDC──▶ Heltec V4 (repeater + MeshAddon) ──UART──▶ Pi Zero 2 W
        A: text CLI + EVT lines            B: JSON lines                 (meshaddond)
                                    ▲
                                    └── LoRa: C: MeshCore group channel (encrypted GRP_TXT)
```

## A. USB CLI (browser ↔ Heltec)

This is the stock MeshCore repeater CLI: lines end with `\r`, and replies are printed as `  -> <reply>`.
MeshAddon adds the `addon …` command family. Every stock command still works (`ver`, `get radio`, `set name`, …).

| Command | Reply | Meaning |
|---|---|---|
| `addon info` | `addon 0.1 slots=8 pi=1 chan=1` | Detect the add-on. Stock firmware replies with "unknown command" or nothing |
| `addon dump` | one-line JSON `{slots:[…],chan:{…},pi:{…},rules:[…]}` | Full config |
| `addon slot <n> <type>,<pin>[+<pin2>],<lpp_ch>,<label>[,k=v;k=v]` | `OK` | Configure slot 1–8 |
| `addon slot <n> none` | `OK` | Clear a slot |
| `addon chan <#name>,<hexkey or ->,<prefix>,<allow or *>` | `OK` | Command channel. Key `-` = derived from the name, same as MeshCore hashtag channels |
| `addon rule add <action>,<command text>` | `OK` | Rule answered by the Heltec. Action = `status`, `slot:N`, `relay:N=1\|0\|t` |
| `addon rule del <command text>` | `OK` | |
| `addon pi <en>,<txpin>,<rxpin>,<baud>` | `OK` | Pi UART link |
| `addon save` | `OK` | Persist to `/addon.json` on SPIFFS/LittleFS |
| `addon events on\|off` | `OK` | Stream `EVT` lines (below) to USB |
| `addon set <slot> <value>` | `OK` | Manually drive an output (relay) |
| `addon test <type>,<pin>[+<pin2>]` | e.g. `21.62 °C`, `pin HIGH → 0 (open)`, `clicked 500 ms` | One-off reading on a pin that isn't saved yet (used by the "Add sensor" wizard). Relay: 500 ms pulse |
| `addon i2c scan <sda>,<scl>` | `found 0x76 0x44` or `found none` | Scan an I²C bus (wizard) |
| `addon pi list` | `OK` (+ `EVT scripts`) | Ask the Pi to rescan its script folder |
| `addon pi run <name> [args]` | `OK` (+ `EVT pireply`) | Run a Pi script from the configurator (test button) |

I²C default pins: **SDA = GPIO3, SCL = GPIO4** (same as MeshCore's `heltec_v4_sensor` env: `ENV_PIN_SDA=3`, `ENV_PIN_SCL=4`).

Sensor types are `ds18b20`, `dht22`, `analog`, `digin`, `counter`, `relay`, `pir` and `i2c`. An `i2c` slot sets the bus pins; the existing
`EnvironmentSensorManager` then auto-detects BME280/BMP280/SHT4x/AHTx0/INA219/… on it.

### EVT lines (Heltec → browser, only when events are on)

```
EVT {"t":"rx","type":"GRP_TXT","snr":7.5,"rssi":-92,"fwd":true}
EVT {"t":"sensor","slot":1,"v":6.81}
EVT {"t":"tele","from":"field-node"}              telemetry request answered
EVT {"t":"chan","ch":"#relay","from":"alice","text":"!get devid relay status"}
EVT {"t":"pireply","text":"devid=… relay=ON"}
EVT {"t":"reply","text":"Port relé: ON"}           Heltec replied on the channel
EVT {"t":"scripts","list":[{"name":"uptime","desc":"…"}]}
EVT {"t":"pi","state":"up","host":"pizero-relay","dir":"/opt/meshaddon/scripts"}  also "down"
```

## B. Heltec ↔ Pi (UART, 115200 8N1, 3.3 V, newline-terminated JSON) — scripts only

Heltec → Pi:
```json
{"t":"hello","fw":"v1.13.0-addon0.2","node":"garage-node"}
{"t":"run","id":17,"ch":"#relay","from":"alice","script":"wake-pc","args":["office"]}
{"t":"list"}                                           ask for a rescan
{"t":"sensors","v":{"1":6.81,"2":1}}                   every 60 s → scripts get MESH_S1, MESH_S2 …
{"t":"ping"}
```
Pi → Heltec:
```json
{"t":"hello","host":"pizero-relay","ver":"0.2","dir":"/opt/meshaddon/scripts"}
{"t":"scripts","list":[{"name":"uptime","desc":"Pi uptime and load"}]}   on hello, on "list", and whenever the folder changes (checked every 15 s)
{"t":"reply","id":17,"script":"wake-pc","text":"WOL sent to office"}     stdout, max 140 chars, newlines → " | "
{"t":"pong"}
```
The link is marked `down` if nothing arrives from the Pi for 30 s.

### Script folder (Pi)
* Default `/opt/meshaddon/scripts`, group `meshaddon`, setgid 2775. Your user is added to the group, so you can just copy files in.
* Every **executable** file is a script. The mesh name is the filename without extension and must match `[A-Za-z0-9_-]{1,24}`.
* Description: a line `# desc: …` within the first 10 lines.
* Run as user `meshaddon`, without a shell. Up to 8 args, each `[A-Za-z0-9_.,:=@+/-]{1,48}`. Timeout 30 s; one instance per script.
* Environment: `MESH_FROM`, `MESH_CHANNEL`, `MESH_NODE`, `MESH_S<n>`.
* Reply: stdout, or stderr if stdout is empty; a non-zero exit becomes `[exit N] …`.

### Channel syntax
| Message | Answered by |
|---|---|
| `!status`, `!temp`, `!port` … (rules) | Heltec |
| `!scripts` | Heltec, from the list the Pi last sent; works even if the Pi is busy |
| `!run <name> [args]` | Pi → reply relayed by the Heltec. If the link is down, the Heltec replies `Pi link is down` |

## C. Mesh side

The command channel is a normal MeshCore group channel (same key derivation as hashtag channels, or an explicit PSK).
The repeater keeps forwarding everything as before. In addition, the add-on overrides `searchChannelsByHash()` and
`onGroupDataRecv()` so it can decrypt **only** that one channel. Messages that start with the prefix, come from an allowed
sender and match a rule (or `scripts`/`run`) are acted on. The reply is sent with `createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, …)` as
`<node name>: <reply>`, which makes it look like a normal channel message in the app.

Telemetry: the repeater's `REQ_TYPE_GET_TELEMETRY_DATA` path calls `sensors.querySensors()`. MeshAddon swaps in an
`AddonSensorManager` (subclass of `EnvironmentSensorManager`) that appends one CayenneLPP field per slot, plus any Pi-provided
values. The MeshCore app ("Telemetry" on the repeater) and the Home Assistant MeshCore integration show these values with no changes needed.

Guest permissions: telemetry needs the admin or guest password, as on stock repeaters. Relay commands over the channel are
gated by `allow` (sender names). That check is **not** cryptographic, since anyone with the channel key can type any name. Use a
private PSK channel for relays, not a public `#hashtag`.
