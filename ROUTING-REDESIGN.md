# ROUTING-REDESIGN.md — RX-priority route-queue + CAD + hash-dedup

**Status:** Design only — not implemented. Sequenced **after** the UART link and
2.4 GHz wideLora BW are proven on hardware (don't stack a routing rewrite on an
unverified bring-up). Tracked as task #1.
**Author:** GrayHatGuy · **Branch:** `T_LORA_QUAD` · 2026-06-06

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
**Do not implement until** the link + 2.4 GHz wideLora are confirmed on hardware.
Files: `src/main.cpp` (radioTask + pipeline), `src/UartLink.cpp` + `RemoteRadio`
(TX_DONE backpressure), `coproc-tlora-dual/src/main.cpp` (non-blocking TX + CAD),
`NodeDB.*` (sibling dedup cache), `LinkProtocol.h` (`TX_DONE` already defined).

**Acceptance:** no RX drops under concurrent full-mesh; no UART overflow; clean
far-side bodies (no marker); loops still prevented (incl. same-channel cross-band
MT/MC twins).
