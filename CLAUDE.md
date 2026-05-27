# CLAUDE.md — Project context for Claude sessions

**This file is the single source of truth for project state. Read it first. Do not generate "handoff documents" — update this file instead.**

---

## What this is

Multi-protocol LoRa mesh bridge on Seeed Xiao ESP32-S3. Bridges Meshtastic, MeshCore, and (stub) Reticulum networks across two radios sharing one SPI bus.

- **Repo:** https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater
- **Local path:** `C:\Users\6r4yh\workspace\Platformio\Projects\xiao esp32 wio sx1262 dual repeater` (note: contains spaces)
- **Owner:** GrayHatGuy — `grayhatguyllc@protonmail.com`
- **Contest:** Seeed/Meshtastic Build-Off 2026, issue #2 at `Seeed-Projects/meshtastic-build-off-2026`

## Current state

| Item | Value |
|---|---|
| **Production line** | `main` @ v8.1 — Phase 0 dual-SX1262, ship-ready, contest deliverable |
| **Investigation branch** | `lr1121-phase1` |
| **Branch tip** | Check with `git rev-parse lr1121-phase1` |
| **Snapshot tag (shared with Seeed)** | `lr1121-bringup-2026-05-26` — mutable, force-push acceptable; bump after material commits |
| **Default build flag** | `LR1121_RX_AUDIT_RUN=0` in `platformio.ini` (clean state) |

## Phase status

- **Phase 0 (v8.1)** — ✅ Shipped. Dual-SX1262 multi-protocol bridge. The contest deliverable.
- **Phase 1** — ⏸️ **HARDWARE-BLOCKED on Seeed Wio-LR1121 module (SKU 113991415).** All firmware-side investigation complete. RX path is non-functional despite TX working. Sensitivity degraded ~40–50 dB versus LR1121 datasheet. Seeed engineering inquiry sent 2026-05-27 with full DOE evidence; **awaiting reply**. Firmware infrastructure is complete on `lr1121-phase1` branch.
- **Phase 2** — 📋 Dual-LR1121. Compile-verified, awaits Phase 1 resolution.

## What was investigated (firmware-side, COMPLETE)

Two full DOE phases against Semtech LR1121 User Manual v2.2 + comparison to stock Meshtastic firmware:

- **Phase A — 12-iteration RFSWx switch-table sweep.** All chip-level RFSWx-capable DIOs (DIO5/6/7/8/10) exhausted. Self-echo RSSI invariant within ~7 dB across all states.
- **Phase B — 5 chip-init runs** (`LR1121_RX_AUDIT_RUN` build flag, values 0/2/3/5/6 bench-tested):
  - Run 0 baseline → `errors=0x0020 = HF_XOSC_START_ERR` persistent at every POR
  - Run 2 `SetRssiCalibration` (UM Table 7-21 600M-2G) → failed; +4 dB AGC shift
  - Run 3 `CalibImage(902, 928)` → failed
  - Run 5 kitchen-sink → failed; **one `RADIOLIB_ERR_CRC_MISMATCH` signal-of-life event**
  - Run 6 Meshtastic-style (DCDC + 2-DIO switch table + `setPreambleLength` before `startReceive`) → failed; **17 dB self-echo reduction** (D7/D8/D10 NC closed a parasitic path)

Runs 1 (pre-standby alone) and 4 (RxBoosted alone) were intentionally skipped — rationale invalidated by Run 0 + mathematically incapable of closing the gap.

**Comparative bench evidence (devastating for Seeed):** Five working LoRa RX paths exist on the same physical bench — 2× LilyGO T3S3 LR1121 (sub-GHz + 2.4 GHz), 1× T-Watch S3 (SX1262 + SX1280). Only the two Wio-LR1121 modules fail. Chip + firmware + RF environment all ruled out. The bug is Seeed-Wio-LR1121-carrier-specific.

## What's blocked on what

- **Phase 1 ship** is blocked on EITHER:
  - Seeed engineering reply with a working fix, OR
  - Hardware pivot to a known-good LR1121 carrier (bare module on Xiao or LilyGO T-Lora Dual on ESP32-C5)

- **No firmware action is required while waiting.** The branch is stable at `lr1121-phase1` tip. Default flag `LR1121_RX_AUDIT_RUN=0` restored.

- **Owner is intentionally NOT sending Seeed any further messages.** No follow-up emails. No partial updates. The 2026-05-27 inquiry stands as the single decisive evidence package.

## Decision tree — when Seeed replies

| Reply content | Action |
|---|---|
| RF switch control mechanism + truth table | Implement in `WioLR1121::begin()`, retest, release v9.1 |
| Per-PCB `SetRssiCalibration` tune values | Implement, retest, release v9.1 |
| Chip-firmware v1.3 errata confirmation | Apply prescribed workaround OR wait for firmware update |
| Hardware design defect confirmation | Pivot to bare LR1121 module on Xiao (preferred) or T-Lora Dual |
| **No reply in 10 business days** | Owner sends ONE polite ping in their own voice (not AI-drafted). After that: pivot to bare LR1121 module. |

## Operational rules (read carefully)

**Shell:**
- Owner runs PowerShell. Bash heredocs (`<<EOF`) do not work in their terminal.
- For multi-line content (commit messages, release notes): write to a file via `Write` tool, then `git commit -F <file>` or `gh release edit --notes-file <file>`.
- The internal Bash tool runs on a Git Bash shim. When using it, prefix with `cd "/c/Users/6r4yh/workspace/Platformio/Projects/xiao esp32 wio sx1262 dual repeater" && ...` due to spaces in path.

**Tools and paths:**
- `pdftotext`: `C:/Program Files/Git/mingw64/bin/pdftotext.exe` — but verify PDF table extractions against a clean screenshot; multi-column cells get OCR-scrambled.
- `pio device monitor --port COM6` is the standard bench monitor command.

**Git:**
- **Never force-push `main`** unless explicitly resetting to a known tag.
- **Force-pushing `lr1121-bringup-2026-05-26` IS expected** — that's its purpose; bump after material commits so Seeed-correspondence links resolve to current state.
- Commit messages: HEREDOC via `git commit -m "$(cat <<'EOF' ... EOF)"`. Always include `Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>`.

**RadioLib quirks:**
- `LR11x0::getErrors()` and `LR11x0::setRssiCalibration()` are `protected` in RadioLib 7.7.0. Access via the `LR1121Access` struct in `WioLR1121.cpp` (using-declaration access-promoting subclass).
- `setRfSwitchTable()` takes `const uint32_t (&)[5]`, not a pointer — must pass the array by name.

## DO NOT (lessons earned)

- **Do not generate "handoff documents."** Update this file instead. One source of truth.
- **Do not draft follow-up emails to Seeed.** Owner has decided no further outreach until Seeed replies. Drafting one anyway makes them look like they're flailing.
- **Do not propose bench iterations "for rigor"** when the technical question is already answered. Runs 1 and 4 were the last casualty of this pattern.
- **Do not propose proactive public-surface updates, README polish, or release-body refreshes** unless owner specifically asks.
- **Do not propose hardware purchases unless asked** for purchase decisions.
- **Do not use TaskCreate/TaskUpdate** for this project — external state tracking via this file and the deep docs is sufficient.
- **Do not "check on the Seeed reply"** — owner is monitoring their own inbox.
- **Do not re-draft the SEEED_EMAIL_DRAFT.md.** Owner edits it directly on GitHub when needed.

## File pointers (read these for depth, do not duplicate them here)

| File | Purpose |
|---|---|
| `LR1121-SPEC.md` | Phase 1 design + complete investigation history |
| `LR1121-RX-INIT-AUDIT.md` | DOE bench plan + per-run results (rev 3 + Run 6 appendix) |
| `SEEED_SUPPORT_INQUIRY.md` | Full bug report sent to Seeed; the decisive evidence package |
| `SEEED_EMAIL_DRAFT.md` | Locked-in email body sent to Seeed engineering 2026-05-27 |
| `SEEED_RECOMMENDATIONS.md` | Tier 1–4 design feedback for Seeed |
| `src/WioLR1121.cpp` | LR1121 driver wrapper — where DOE code lives, gated by `LR1121_RX_AUDIT_RUN` |
| `src/WioSX1262.cpp` | SX1262 driver wrapper — Phase 0 production code |
| `src/LoraRadio.h` | Abstract base class — `WioSX1262` and `WioLR1121` both implement |
| `platformio.ini` | Build config — `RADIO_PROFILE` + `LR1121_RX_AUDIT_RUN` flags documented in comments |

## Reference documents (external)

- **Semtech LR1121 User Manual v2.2** — direct PDF: https://semtech.my.salesforce.com/sfc/p/#E0000000JelG/a/RQ00000DClgP/D.pNG5l4FviPI634eCx8GFURZEwDO2ZBA33MpriB_FU · stable page: https://www.semtech.com/products/wireless-rf/lora-connect/lr1121
- **Semtech LR1121 Datasheet v2.1** — direct PDF: https://semtech.my.salesforce.com/sfc/p/#E0000000JelG/a/RQ0000093ZiP/RV4Ba6LROsFrFjnAAVK2av5W11RGmCms_3Q2cyKHdDA · stable page: https://www.semtech.com/products/wireless-rf/lora-connect/lr1121
- **Seeed Wio-LR1121 Module Datasheet v1.0** — https://files.seeedstudio.com/wiki/Wio-LR1121/Wio-LR1121_Module_Datasheet_v1.0.pdf · wiki: https://wiki.seeedstudio.com/wio_lr1121_module/
- **Meshtastic firmware LR1121 init (known-good reference)** — `meshtastic/firmware` → `src/mesh/LR11x0Interface.cpp` + `variants/esp32s3/tlora_t3s3_v1/rfswitch.h`

## Update protocol for this file

- Update **only** when material state changes (Seeed replies, branch tip lands a significant commit, phase status changes, owner makes a strategic pivot).
- Do not snapshot — overwrite in place.
- Owner can edit directly; AI can edit when asked or when state genuinely changes.
- If you feel the urge to create a NEW doc summarizing state, edit this one instead.
