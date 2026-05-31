# Upstream Bugs & PR Candidates

Notes on bugs discovered in upstream projects during this project's bench testing that warrant a PR or issue report when time permits. Tracked here so they aren't lost.

---

## Meshtastic firmware — Region change does not re-derive bandwidth from preset

**Status:** Discovered 2026-05-30 during Wio-LR1121 Phase 1 bring-up bench session. Workaround applied locally. **Not yet reported upstream.**

**Affected:** Meshtastic firmware `2.7.15.567b8ea` on LilyGO TLORA_T3_S3 (T3S3 LR1121 variant). Likely affects all hardware platforms — bug is in shared LoRa config logic.

**Repo:** https://github.com/meshtastic/firmware

### Symptom

When changing `lora.region` via the Meshtastic CLI (e.g., US → LORA_24 or vice versa) while `lora.usePreset = true` and `lora.modemPreset` is set, the firmware does **not** re-derive `lora.bandwidth` from the new region's preset definition.

Example: T3S3 starts on US region with LongFast → bandwidth = 250 (= 250 kHz). User runs:

```
meshtastic --port COM5 --set lora.region 4   # LORA_24
```

After reboot, T3S3 reports:
```
lora.region: LORA_24
lora.usePreset: true
lora.modemPreset: LONG_FAST
lora.bandwidth: 250          # ← WRONG, should be 800 (= 812.5 kHz per 2.4G LongFast preset)
lora.spreadFactor: 11
lora.codingRate: 5
```

The mismatched bandwidth is silently used at TX/RX. Any companion device configured per the *expected* 2.4 GHz LongFast preset (BW 812.5 kHz) will fail to demodulate the T3S3's packets because BW mismatch (250 vs 812.5) prevents LoRa correlator lock.

### Real-world failure mode

If the user does NOT manually inspect `lora.bandwidth` after a region change, they see:
- T3S3 successfully boots and TXes at "2.4 GHz LongFast"
- Companion radio at the documented 2.4 GHz LongFast params (812.5 kHz) reports zero RX
- User chases firmware/RF issues for hours when the actual problem is upstream Meshtastic dropping the BW update.

### Workaround (verified working)

Four-step manual sequence:

```
1. meshtastic --port COM5 --set lora.usePreset false
2. meshtastic --port COM5 --set lora.bandwidth <target enum>
     # e.g. 800 for 2.4 GHz LongFast (= 812.5 kHz)
3. Reboot device (CLI reboots automatically on save, or trigger one)
4. meshtastic --port COM5 --set lora.usePreset true
```

Step 1 disables the preset (so manual BW takes effect). Step 2 writes the correct BW. Step 4 re-enables preset mode — the firmware then honors the correctly-set BW.

### Suggested fix (high-level)

In the region/preset config handler (probably `src/mesh/RadioInterface.cpp` or `src/configuration.h` in the Meshtastic firmware repo):

- When `lora.region` changes while `lora.usePreset = true`, the firmware should:
  - Look up the region's preset table for the current `modemPreset`
  - Overwrite `lora.bandwidth`, `lora.spreadFactor`, `lora.codingRate` with the new region's preset values
  - Persist to NVS

Currently it appears to update region but skip BW/SF/CR re-derivation.

### Suggested PR title

"Fix: re-derive bandwidth/SF/CR from preset on region change"

### Reproduction steps for the issue report

1. Flash Meshtastic firmware ≥ 2.7.15 on a T3S3 (or other LR1121-equipped board)
2. Set US region with LongFast preset (default for US)
3. `meshtastic --port <port> --get lora` — confirm `bandwidth: 250`
4. `meshtastic --port <port> --set lora.region 4` (LORA_24)
5. Wait for reboot
6. `meshtastic --port <port> --get lora` — **bug:** `bandwidth: 250` (still), should be `800` to match 2.4G LongFast preset

### Local impact

Documented for the project's own benefit. Once the bench fully validates 2.4 GHz, file the upstream issue with reproduction steps + suggested fix. Until upstream is patched, anyone running our `docs/testbed/HACKRF-DIAGNOSTIC-PLAN.md` Test 0b at 2.4 GHz on a T3S3 must follow the manual workaround sequence above before trusting the T3S3 as a known-good reference radio.

---

## Template for future entries

```
## <Upstream project> — <one-line summary>

**Status:** <date discovered>; <action status — workaround applied / reported / merged / etc>
**Affected:** <project>, version, platform
**Repo:** <github URL>

### Symptom
<what the user sees>

### Workaround
<steps if any>

### Suggested fix
<high-level pointer to relevant code area>

### Reproduction steps
<numbered list>

### Local impact
<how it affects this project>
```
