# V8 Spec — Vanilla Firmware, Fully Portal-Configured

Status: **ready — all 3 open decisions confirmed (see end).** No code yet.
Tracked as task #23.

## Goal

Ship a single distributable `.bin` that anyone can flash — no PlatformIO, no
build flags, no source build — and configure **entirely through the WiFi
captive portal**: region, per-radio protocol, RF parameters, channels, and
identity.

The `platformio.ini` build flags remain available as *optional* pre-seeding
for people who build from source. Absent them, the firmware boots
unconfigured and drops straight into the captive portal.

## Motivation

Not everyone building this bridge will compile from source with custom build
flags. A "vanilla bin" — flash and configure over WiFi — is the difference
between a hobby project and something a non-developer can deploy. It also
makes international deployment practical: the same image works in any region
because the region is a portal setting, not a compile-time decision.

## Components

### 1. Null defaults

- Drop the `WioSX1262.h` hard fallback chain (`915.0f` / `SF9` / `0x12` …).
- If a radio's `LORA_RADIO*` build flags are unset, that radio resolves to
  **null / unconfigured** — no guessed values.
- A null radio at boot (no build flag *and* no NVS value) forces the captive
  portal. The bridge does not start until both radios are configured.
- Result: a firmware image built with **no `LORA_RADIO*` flags** first-boots
  every device straight into the portal — a true "configure-me" image.

### 2. Per-radio protocol picker (captive portal)

- A `<select>` per radio: **Meshtastic / MeshCore / Reticulum / Custom /
  None**. **None** disables that radio slot — useful as a debug tool (bring
  the bridge up on a single radio) or to park a slot until a 2.4 GHz radio
  is fitted. A radio set to None is skipped at `setup()`; the other radio
  still requires a valid config.
- MT / MC / RNS apply a vetted RF preset bundle, derived from
  `region + protocol`.
- **Custom** = the user enters the full RF plan: frequency, bandwidth,
  spreading factor, coding rate, sync word, TX power.
- The bridge derives its *decoder* from the sync word (`0x2B`→MT, `0x12`→MC,
  `0x42`→RNS). A Custom radio with an unrecognised sync word receives RF but
  the bridge cannot decode it — the Custom user's risk.
- Form behaviour (small inline JavaScript): a preset protocol shows only the
  channel name/key fields; **Custom** reveals the six RF fields behind an
  explicit warning banner.

### 3. Region support — Tier 1

- A **global** device region selector in the portal: US, EU_868, EU_433,
  ANZ, CN, JP, IN, KR, RU, plus Custom/Other.
- A region defines the sub-GHz band. For Meshtastic radios the firmware
  computes the **channel-slot frequency** from `region + modem-preset +
  channel-name` (Tier 2) and pre-fills the frequency field with the result.
  The field stays **editable** — the user can override the computed value
  with the exact frequency their network uses. For MeshCore / Reticulum /
  Custom, `region + protocol` pre-fills a flat band default (Tier 1), also
  editable.
- The Meshtastic channel-slot hash formula must be verified against
  `meshtastic/firmware` before this ships.
- **Override visibility.** When the user edits the frequency away from the
  Tier 2 computed slot, the portal shows an inline hint
  (`computed: 906.875 \xe2\x80\x94 overridden`) so an accidental typo is obvious
  while a deliberate override is never blocked. Tier 2 is a smart default
  for a Meshtastic-centric deployment, not a lock.
- Region governs **sub-GHz radios only**. A future 2.4 GHz radio is
  **region-exempt** — the 2.4 GHz ISM band (2400–2483.5 MHz) is licence-free
  worldwide, so that radio resolves its frequency from the 2.4 GHz band
  regardless of the region setting.
- TX power carries region-aware sane defaults / caps (regulatory: US
  +30 dBm / 100 % duty, EU +27 dBm or less / 10 % duty).

### 4. Per-radio channels

Already delivered in v2 (v7.0): each radio carries its own channel name +
key, portal-editable. Default = the public channel for that protocol;
override with a private key. Reticulum has no channel key.

### 5. Identity derived from MAC

- The default Meshtastic node ID and the captive-portal SSID are derived
  from the ESP32's MAC address, so every vanilla device is unique out of the
  box — no `0xB16B00B5` / `LoRa-Bridge-B5` collisions when several units are
  flashed from the same image.
- The user can still override the identity in the portal.

### 6. Compile-time constants (not portal, not flags)

- **Preamble length** — 8 symbols, universal.
- **TCXO voltage** — 1.8 V, a property of the Wio SX1262 hardware. A user
  cannot be expected to know their module's TCXO voltage; it is a board
  fact. Different radio hardware = rebuild.

### 7. BridgeConfig schema v3 → v4

New fields:
- global `region`,
- per-radio `protocol`,
- per-radio RF (`frequency`, `bandwidth`, `sf`, `cr`, `syncWord`,
  `txPower`) — used for the Custom path and as the resolved values for
  preset paths.

`begin()` migrates a v3 blob forward. `setup()` resolves each radio's
`LoraConfig` at runtime from `BridgeConfig` instead of compile-time macros;
build-flag values become first-boot defaults only.

### 8. Runtime validation (captive portal)

In `handleSave()`, for the Custom path: clamp frequency to the SX1262's
tuning range, sanity-check SF (5–12), CR (5–8), and bandwidth. This
complements task #22 (compile-time validation of the build-flag defaults) —
the two are not redundant: #22 checks defaults at build time, this checks
portal-entered values at save time.

## Safety analysis

- This **reverses the "config-time only" decision for RF** — accepted. The
  MT/MC/RNS presets are vetted and safe; only the Custom path is dangerous,
  and it sits behind an explicit warning banner.
- It is **not a brick risk**: the captive portal runs *before* radio init,
  and the BOOT-button / serial-character re-entry trigger already exists. A
  bad Custom RF config → reboot, re-enter portal, fix.

## Build staging

The change is F4 + v2 combined in size; schema, `setup()` and the portal
must change together, so it does not split into independently-compiling
pieces. Planned stages (one atomic landing):

- **A** — `BridgeConfig` schema v4 (region + per-radio protocol + RF) +
  v3→v4 migration.
- **B** — null-default behaviour; MAC-derived identity + SSID; `setup()`
  resolves `LoraConfig` at runtime; null radio → portal.
- **C** — region + preset tables (9 regions); Meshtastic channel-slot
  frequency computation (Tier 2, hash verified against `meshtastic/firmware`);
  radio-channel resolution extended to derive RF from
  `region + protocol [+ modem-preset + channel-name]`.
- **D** — captive-portal form: region selector, per-radio protocol
  dropdown, Custom RF fields, JS show/hide, runtime validation.
- **E** — docs (README, CHANGELOG); release as **v8.0**.

## Open decisions — confirm before any code

- **(a)** ~~Offer **None / disabled** as a 5th portal protocol option?~~
  **CONFIRMED** — yes. None disables a radio slot; serves as a debug tool
  (single-radio bring-up) and parks a slot for a future 2.4 GHz radio.
- **(b)** ~~Which regions ship in the v1 preset table?~~ **CONFIRMED** —
  fuller list: US, EU_868, EU_433, ANZ, CN, JP, IN, KR, RU — plus
  Custom/Other for everything else.
- **(c)** ~~Confirm Tier 1 for v1.~~ **CONFIRMED** — **Tier 2**: full
  Meshtastic `region + modem-preset + channel-name` channel-slot frequency
  computation. The slot-hash formula must be verified against
  `meshtastic/firmware` before stage C lands. The frequency field stays
  editable (Tier 1 behaviour is retained as an override), but the default
  is now slot-computed instead of a flat region pre-fill.

## Out of scope / future

- The 2.4 GHz radio itself (LR1121 / SX1280 wrapper) — separate roadmap
  item. This spec only ensures the config model *accommodates* it (region
  exemption for 2.4 GHz radios).
