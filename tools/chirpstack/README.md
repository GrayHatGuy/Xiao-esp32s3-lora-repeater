# ChirpStack integration — device-profile templates + payload codec

Everything needed to ingest the bridge's **LoRaWAN ABP uplinks** into a
[ChirpStack](https://www.chirpstack.io/) v4 server: importable **device-profile
templates** (one per region) and the JavaScript **payload codec**.

The bridge's keyed ABP encoder (v8.4) re-emits mesh / Custom traffic as a valid
LoRaWAN **Class-A, ABP** uplink; a separate gateway forwards it to ChirpStack.
These templates describe that device so the codec decodes the payload server-side.

## Layout

```
tools/chirpstack/
└── vendors/grayhatguy/
    ├── vendor.toml                         # vendor identity + device list
    ├── devices/xiao-lora-bridge.toml       # firmware 8.4 -> profiles + codec
    ├── profiles/
    │   ├── xiao-bridge-abp-US915.toml      # one ABP profile per region
    │   ├── xiao-bridge-abp-AU915.toml
    │   ├── xiao-bridge-abp-AS923.toml
    │   └── xiao-bridge-abp-EU868.toml
    └── codecs/
        ├── xiao-bridge.js                  # = tools/chirpstack-codec.js (kept in sync)
        ├── test_decode_xiao-bridge.json
        └── test_encode_xiao-bridge.json
```

> **Region status:** **US915** is hardware-bench-verified. **AU915 / AS923 / EU868**
> are authored from RP002-1.0.3 but **not yet bench-verified**. **OTAA** is not
> included — these profiles are **ABP only** (`supports_otaa = false`); an OTAA
> profile will be added when the OTAA firmware ships in a later release.

## Importing the templates

**Bulk (template repo):** point ChirpStack's importer at this folder:

```
chirpstack -c /etc/chirpstack import-device-profiles -d /path/to/tools/chirpstack
```

Then in the web UI: **Device profiles → Add → "Select device-profile template" →
GrayHatGuy → Xiao ESP32-S3 Dual-SX1262 LoRa Bridge → FW 8.4 → (your region)**.

**Manual (no import, fastest for 1–2 devices):** create a Device Profile by hand
using the values from the matching `profiles/*.toml`
(MAC version **1.0.3**, region e.g. **US915**, **Class A**, **ADR off**), then open
its **Codec** tab, set **"JavaScript functions"**, and paste the whole
[`codecs/xiao-bridge.js`](vendors/grayhatguy/codecs/xiao-bridge.js)
(identical to [`tools/chirpstack-codec.js`](../chirpstack-codec.js)).

## Creating the device (per-device keys go HERE, never in the template)

Create the device under the profile and enter the **ABP session** matching the
bridge's portal (or build-flag) credentials:

| ChirpStack field | Value | Where it comes from |
|---|---|---|
| Device address (DevAddr) | e.g. `01000001` | portal "LoRaWAN ABP devices" slot, or `BRIDGE_LW_ENC_DEVADDR` |
| Network session key (NwkSKey) | 32 hex | portal slot, or `BRIDGE_LW_ENC_NWKSKEY` |
| Application session key (AppSKey) | 32 hex | portal slot, or `BRIDGE_LW_ENC_APPSKEY` |
| Frame-counter validation | **disabled** (or persist) | the bridge reserves FCnt in blocks |

## Per-device codec options (ChirpStack Device → Variables)

The codec reads two optional device **variables** so one pasted codec serves
mixed devices without edits:

| Variable | Set to | Effect |
|---|---|---|
| `source_tag` | `true` | this device has the portal "Prepend source tag" enabled — the codec splits the leading `[proto][srcId]` tag into `data.source` |
| `station_fport` | e.g. `13` | the FPort whose payload is a raw weather-station frame (routed to `decodeStation()`) |

If a variable is absent, the codec's `DEFAULT_SOURCE_TAG` / `DEFAULT_STATION_FPORT`
constants apply.

## Weather-station payload

`decodeStation()` in the codec is a **placeholder** layout — the bridge forwards a
Custom raw-LoRa station's bytes verbatim, so only the station's own datasheet
defines the format. Edit `decodeStation()` to your station's real byte layout
(field order / width / endianness / scaling), then update
`test_decode_xiao-bridge.json` to match.
