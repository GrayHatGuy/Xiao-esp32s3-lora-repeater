# CLAUDE.md — project operating manual

**Project:** XIAO ESP32-S3 dual-radio LoRa mesh bridge (sub-GHz ↔ 2.4 GHz cross-band; Meshtastic ↔ MeshCore ↔ Reticulum-stub).
**Repo:** https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater
**Owner:** `GrayHatGuy` (pseudonym), `grayhatguyllc@protonmail.com`. **Real name is ephemeral — never persist it to docs.**
**Shell:** PowerShell (Windows). HEREDOC commit messages via the Bash tool; build/flash via PlatformIO (`pio`).

> **This is the `CORE1121` branch.** Start with **[`CORE1121.md`](CORE1121.md)**.
> The full Wio-LR1121 investigation that this branch builds on is the *background* —
> it lives on the [`lr1121-phase1`](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/tree/lr1121-phase1)
> branch and is intentionally **not duplicated here**.

---

## Branches

| Branch | What it holds |
|---|---|
| **`main`** | Phase 0 — dual Wio-SX1262, sub-GHz only. Ships at v8.1. |
| **`lr1121-phase1`** | Phase 1 — Wio-LR1121 bring-up **and the full RX-deficit baseline** (Seeed correspondence, `LR1121-RX-INIT-AUDIT.md`, testbed, measurement method, detailed `CLAUDE.md`). The **baseline-of-record**; keep it intact until the RX-deficit question is resolved. |
| **`CORE1121`** *(this branch)* | Radio 2 swapped to the **WaveShare Core1121** (same LR1121 chip, published schematic) as a board-vs-chip control. See [`CORE1121.md`](CORE1121.md). |

**Plan:** eventually merge all branches to `main` once the Wio-LR1121 RX-deficit question is resolved.

## The goal

Phase 1: one XIAO ESP32-S3 hosting two LoRa radios bridging Meshtastic ↔ MeshCore across bands —
**R1 = Wio-SX1262** (sub-GHz, healthy reference radio), **R2 = LR1121** (dual-band). The open
issue is the LR1121's **marginal sub-GHz RX deficit** observed on the Seeed board. The Core1121
(this branch) is the board-vs-chip control that decides whether that deficit is a Seeed *board*
flaw or an LR1121 *chip* issue — full reasoning and status in [`CORE1121.md`](CORE1121.md).

## Rules of engagement (firm)

- **Don't guess** — cite the datasheet/schematic/code for electrical/timing/safety/RF-switch
  claims; say "I don't know" otherwise. Reverts and "no action needed" conclusions need the
  same citation.
- **One variable per experiment.** No mid-experiment edits that contaminate the record.
- **No RF-switch / PA / timing-sensitive changes without a written, datasheet/schematic-cited
  rationale.**
- **Never cable a high-power source into an RX front end** (LR1121 abs-max RF input = +10 dBm).
- **Owner reviews all outbound** (Seeed correspondence, PRs) — drafts only. **Accepted edits
  ship — don't re-ask.**
- **Pseudonym in docs; never the real name. Never force-push `main`.**

## Key docs (this branch)

1. **[`CORE1121.md`](CORE1121.md)** — the Core1121 summary, board facts, wiring, bring-up plan.
2. [`README.md`](README.md) — build / flash / wiring / captive-portal config.
3. [`docs/datasheets/waveshare/CORE1121-RF-SWITCH.md`](docs/datasheets/waveshare/CORE1121-RF-SWITCH.md) — RF-switch truth table, schematic-derived.
4. [`docs/REFERENCES.md`](docs/REFERENCES.md) — datasheet / reference index.
5. [`CHANGELOG.md`](CHANGELOG.md) — release history. [`V8-SPEC.md`](V8-SPEC.md) — Phase-0 portal-config spec.
6. **Background:** the `lr1121-phase1` branch (full Wio-LR1121 record).

## Quick recovery for a fresh session

1. Read this file, then `CORE1121.md`.
2. `git log --oneline -15` to confirm HEAD.
3. For the Wio-LR1121 history/baseline, switch to `lr1121-phase1` (or browse it on GitHub).
4. **Confirm the live bench state with the owner before assuming the firmware matches the docs.**
