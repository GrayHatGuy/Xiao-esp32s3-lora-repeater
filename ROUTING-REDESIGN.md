# ROUTING-REDESIGN.md — RX-priority route-queue + CAD + hash-dedup

**Status:** IMPLEMENTED on branch `T_LORA_QUAD_ROUTE` (off `T_LORA_QUAD` @ `3b06f3c`),
**code-only** per owner directive 2026-06-07 — both firmwares build green; on-air
behaviour is intentionally **UNPROVEN** (no bench validation). Covers task #1 (this
doc) and task #2 (full-mesh airtime throttle, §6). The original sequencing note
(§7, "implement after the link + 2.4 GHz are proven") was **waived** by the owner;
merge to `T_LORA_QUAD` and hardware validation remain later owner decisions.
**Author:** GrayHatGuy · **Branch:** `T_LORA_QUAD_ROUTE` · designed 2026-06-06, implemented 2026-06-07

---

## 1. Motivation

The 4-radio bridge currently ships two stopgaps:

- **Loop prevention** prepends a `[MT]/[MC]/[rns]` marker to the decoded text and
  drops any RX that already carries one (`src/main.cpp`, loop-check ~L478). Works,
  but it pollutes the far-side message, only applies to text, and false-drops a
  genuine user message that happens to start with `[MT`.
- **TX** uses RadioLib's **blocking** `transmit()`. A multi-hundred-ms SF11/SF10
  send freezes `loop()`, so the radio stops receiving and — on the co-processor —
  stops draining the UART link, dropping RX and overflowing the link RX ring
  (only *mitigated* by the 4096 B bump in commit `4af3663`).

**Owner priority:** *ingest as much RX as possible; never let a TX clobber an
inbound packet.* TX latency is acceptable in exchange. That is **RX-priority
CSMA** — the model real meshes (Meshtastic / MeshCore / Reticulum) use.

## 2. Pipeline

```
        ┌──────── per radio (R1..R4) — default state is RX ────────┐
RX IRQ ─► enqueue RAW packet, return to RX immediately (never blocks)
        └─────────────────────────┬────────────────────────────────┘
                                   ▼
                   decode (MT / MC / …) → body + srcId
                                   ▼
              HASH-DEDUP   h = hash(body [+ srcId])
              seen in TTL window? ── yes ─► DROP (loop)
                                   │ no → record h
                                   ▼
            fan out to per-DESTINATION route queues (host RAM / PSRAM)
                                   ▼
  TX SCHEDULER: pop a dest's queue only when
       { CAD clear }  AND  { radio idle }  AND  { co-proc TX_DONE, if remote }
                                   ▼
                    transmit one packet → return to RX ASAP
```

## 3. Components

### 3.1 Hash dedup — replaces the marker
- On RX after decode: `h = hash(decoded_body + srcId)` (FNV-1a / CRC32) into a
  **TTL-windowed seen-cache** (~30–60 s). In cache → loop, **drop**; else record
  and bridge the **clean** body (no prepend).
- **Hash the decoded body, not raw bytes** — across a cross-protocol re-encode
  every on-air byte changes; only the *content* is invariant (which is what the
  marker rode on).
- **Fold `srcId` into the hash** so identical text from *different* senders isn't
  false-dropped. MT exposes `[src:4][packet_id:4]` (`MeshDecoderDebug.h:195`);
  **MeshCore GRP_TXT may lack a stable per-sender id** → content-only there,
  accept the higher collision chance.
- **Storage:** a sibling cache to `NodeDB`, reusing its mutex + fixed-table / LRU
  pattern (`NodeDB.cpp`).
- **Bonus over the marker:** also dedups the *same* packet heard on two radios;
  removes the `[MT`-prefix false-drop and the body pollution.

### 3.2 Route queue
- Per-destination, **host-side**. The XIAO ESP32-S3 has **PSRAM** (boot log:
  `psramInit() enabled`), so depth can be **large** (PSRAM-backed).
- The binding bound is **max-age** (drop messages older than *T* so we never
  deliver stale traffic), with a generous **depth ceiling** as a backstop.
  Policy: drop-oldest / expire.

### 3.3 TX scheduler + readiness (listen-before-talk)
- **CAD** (Channel Activity Detection) before every TX — RadioLib `scanChannel()`
  on the SX126x (R1/R2, host) and LR11x0 (R3/R4, co-proc). Channel active →
  **don't TX**, stay in RX, requeue. This is the "don't clobber RX" guarantee.
- **Remote (R3/R4)** TX additionally gated on the co-proc's **`TX_DONE`** frame
  (already defined in `LinkProtocol.h`, C→H): never hand the UART a frame the
  co-proc can't yet take.

## 4. Keystone dependency — non-blocking co-proc TX
`coproc-tlora-dual/src/main.cpp` must move from blocking `transmit()` to
`startTransmit()` + a **TX-done IRQ** (mirror the existing IRQ RX) + a small
per-radio TX queue. Until then a blocking R3 TX makes **R4 deaf and stalls the
UART** — the single biggest RX-loss source. CAD on the co-proc only helps once TX
is non-blocking.

## 5. Consequence — the UART-overflow class dissolves
With TX gated on CAD + `TX_DONE`, the host **never writes a UART TX frame faster
than the co-proc transmits.** Buffering moves out of the 256/4096 B UART ring into
the **bounded host-RAM queue** — the overflow bug class disappears *by
construction*, and the `4af3663` buffer bump becomes moot.

## 6. Constraints & caveats (grounded — not blockers)
- **Half-duplex is physical.** A radio is deaf during its *own* TX. CAD + short TX
  windows minimize missed RX; they cannot zero it.
- **CAD adds per-TX latency** (accepted by design).
- **Reordering:** queuing across a busy vs. free destination may reorder relative
  to arrival — fine for mesh text.

## 7. Sequencing & touch-points
~~Do not implement until the link + 2.4 GHz wideLora are confirmed on hardware.~~
**Waived (owner, 2026-06-07): implemented code-only, on-air unproven.**

## 8. As-built map (T_LORA_QUAD_ROUTE)
- **Keystone — non-blocking co-proc TX + CAD + TX-done IRQ:**
  `coproc-tlora-dual/src/main.cpp` (per-radio TxJob ring; `scanChannel` CAD; DIO9
  TxDone/RxDone disambiguated by `g_txInFlight`; every `MSG_TX` answered by one
  `MSG_TX_DONE`).
- **Non-blocking TX + CAD radio interface:** `src/LoraRadio.h`
  (`scanChannel`/`startTransmit`/`txDone`/`finishTransmit`, blocking defaults);
  real impls in `src/WioSX1262.cpp` (releases the SPI mutex for the on-air time)
  and `src/RemoteRadio.cpp` (gated on the co-proc `MSG_TX_DONE`).
- **TX_DONE backpressure:** `src/UartLink.*` (`armTx`/`txDone`/`txStatus`).
- **Hash dedup (replaces the marker):** `src/DedupCache.*` — sibling of `NodeDB`
  (NodeDB itself is now write-only). Recorded on RX and on every emission.
- **Per-destination route queue:** `src/RouteQueue.*` (PSRAM-backed, age-bounded,
  drop-oldest).
- **Pipeline + TX scheduler + airtime throttle:** `src/main.cpp`
  (`ingestAndFanout`, `enqueueTextForDest`, `enqueueReticulumForDest`,
  `estimateAirtimeMs`, the RX-priority `radioTask`).
- `LinkProtocol.h` `MSG_TX_DONE` was already defined and is now used.

**Acceptance (to verify on hardware — NOT yet validated):** no RX drops under
concurrent full-mesh; no UART overflow; clean far-side bodies (no marker); loops
still prevented (incl. same-channel cross-band MT/MC twins). **Known accepted
limitation:** MeshCore GRP_TXT has no stable per-sender id, so its dedup is
content-only — two *different* MeshCore senders transmitting identical text
within the TTL window will have the second copy dropped from the bridge (the
message still reaches its own MeshCore mesh). A future improvement could fold the
MeshCore sender-MAC prefix into the hash (its semantics need confirming first).

## 9. ⚠️ Foundational risk — co-proc RX runs on RadioLib 7.7.0 LR11x0
**The co-processor's R3/R4 are LR1121s driven by RadioLib 7.7.0**
(`coproc-tlora-dual/platformio.ini`). A separate bench session (2026-06-07,
WaveShare **Core1121**, `CORE1121` branch CLAUDE.md §0.11) established that
**Semtech's official `lr11xx_driver` COMPLETES LoRa RX on this silicon (RX_DONE +
CRC-OK, 149 B @ 910.545 MHz, −74 dBm) while the RadioLib build never completes a
packet** — and that all RadioLib-side knobs (TCXO recal, freq offset, RSSI/AGC
cal, sync, RF-switch) were bench-tested and ruled out. The same RX deficit
appears on both the Seeed Wio-LR1121 and the WaveShare Core1121, i.e. it tracks
**RadioLib's LR11x0 RX path**, not the board.

**Implication for this redesign:** the host SX1262 side (R1/R2) and all the
software logic here (dedup, route queue, scheduler, loop prevention) are
unaffected — but the co-proc **R3/R4 RX may not function at all** on RadioLib
7.7.0. If so, R3/R4 could end up effectively **TX-only** (the deficit is RX
completion; TX likely still keys up), which makes the RX-priority benefit and the
2.4 GHz CAD moot for those slots. **The redesign neither causes nor fixes this** —
it rides above the `LoraRadio` interface, so it will work unchanged on top of a
*working* LR11x0 RX. Do not mis-blame the routing code at bench time.

**Decisive control (owner has the board):** run the LR1121 OEM example on the
**T-Lora-Dual** — it pins **RadioLib 7.4.0**. 7.4.0 RX works ⇒ a 7.4.0→7.7.0
**regression** (downgrade the co-proc, reconcile the unified-IRQ API, file the
upstream PR — `CORE1121` task #5). 7.4.0 fails too ⇒ RadioLib-LR11x0-wide ⇒ the
fix is to **port R3/R4 to Semtech `lr11xx_driver`**, keeping RadioLib for the
SX1262s. That port is scoped in [`COPROC-LR1121-DRIVER-PORT.md`](COPROC-LR1121-DRIVER-PORT.md).
The Phase-2 pivot premise ("the T-Lora-Dual Factory firmware has working RX+TX")
must be re-validated against *our* 7.7.0 co-proc build, which is not the Factory
sketch.
