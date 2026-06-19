# BENCH-v8.5.md — "2xiao_4sx1262" bench verification

双小型处理器全向四路射频亚千兆赫兹全向桥接中继器

Bench plan for **v8.5**: two Xiao dual-SX1262 boards joined by a UART crossover into one
**4-radio sub-GHz bridge** (host + radio co-processor), with a per-radio routing matrix. All four
radios are SX1262 (sub-GHz only).

> Status: **proposed plan** — review/finalize the test list + the gating set, then we run it and tag
> v8.5. Tag is owner-gated; nothing is pushed until the gating set passes and you OK it.

---

## 1. Rig & conventions

**Hardware**
- **DUT** = the 2-board bridge: **Host (Board A)** + **Co-processor (Board B)**, joined by the UART
  crossover. Host runs `xiao_esp32s3`; co-proc runs `xiao_coproc_sx1262`.
- **Nodes**: 2+ Meshtastic, 2+ MeshCore (real radios).
- **Sniffer** = your spare Xiao dual-SX1262 bridge. Flash `bench_lw_sniffer` to capture LoRaWAN
  (`evt=RX proto=LW` + `evt=LWRAW raw=…`), or a stock bridge build to RX-log / generate stimulus.

**UART crossover wiring** (same pins both boards; the cable does the crossover):
```
Board A (host)  D6 (GPIO43, TX) ──────► D7 (GPIO44, RX)  Board B (co-proc)
Board A (host)  D7 (GPIO44, RX) ◄────── D6 (GPIO43, TX)  Board B (co-proc)
Board A (host)  GND ──────────────────── GND             Board B (co-proc)
```
Power each board over its own USB (or share 5V). The link is `Serial1`; the USB-CDC `Serial` stays
the debug console on both boards.

**RF discipline** (the four radios are co-located → they desense each other):
- Region **US915**. **TX ≤ 10 dBm**. Antennas separated; all antennas connected before power.
- Give **each radio a distinct frequency** (real deployments separate the two boards physically).

**Suggested sub-GHz frequency plan** (adjust to your nodes):

| Role | Freq (MHz) | BW (kHz) | SF | Sync |
|---|---|---|---|---|
| Meshtastic LongFast (public) | 906.875 | 250 | 11 | 0x2B |
| Meshtastic private | 905.0 | 250 | 11 | 0x2B (distinct PSK) |
| MeshCore public | 910.525 | 62.5 | 7 | 0x12 |
| MeshCore private | 909.0 | 62.5 | 7 | 0x12 (distinct key) |
| LoRaWAN uplink (Device 1) | 903.9 | 125 | 7 | 0x34 |
| LoRaWAN uplink (Device 2) | 904.1 | 125 | 7 | 0x34 |
| LoRaWAN **US915 RX2** (downlink listen) | 923.3 | 500 | 12 | 0x34 |

**Decode**: `tools/lw-verify.py` (offline MIC-verify + decrypt — no ChirpStack needed). Full
ChirpStack ingestion is the colleague's separate v8.4 Tier-C item (give them DevAddr `0x01000001` +
the bench keys).

---

## 2. Build & flash

```
# Host (Board A) — use _v1_1 if this board's Radio-2 edge module silkscreen reads V1.1
pio run -e xiao_esp32s3 -t upload --upload-port COM_A

# Co-processor (Board B) — note -d points at the subproject; _v1_1 for a V1.1 edge module
pio run -d coproc-xiao-sx1262 -e xiao_coproc_sx1262 -t upload --upload-port COM_B
```
Then monitor the host: `pio device monitor --port COM_A`. Configure the 4 radios + routing matrix in
the captive portal (SoftAP `LoRa-Bridge-XX` @ 192.168.4.1), Save, reboot.

---

## 3. Tests

Each test lists **Setup → Do → PASS**. Gating tests must pass to tag v8.5 (see §4).

### Group 0 — Bring-up & regression  *(all GATING)*

**0.1 Link up**
- Setup: both boards flashed + crossover wired; R3/R4 enabled in the portal.
- Do: power both, watch the host monitor.
- PASS: host prints `[UartLink] co-processor READY` and `[coproc] R3 cfg ok: …` / `[coproc] R4 cfg
  ok: …` echoing the freq/SF you set; host prints `[R3] remote begin -> sent …` / `[R4] …`.

**0.2 Config echo**
- Do: after Save+reboot, read the boot `[BridgeConfig]` dump.
- PASS: all four radios listed with the protocol / frequency / `route=0x..` you configured.

**0.3 Do-no-harm (single board = v8.4.1)**
- Setup: **one board only**, R3/R4 = None (R1 = MT, R2 = MC), no co-proc attached.
- Do: normal MT↔MC bench.
- PASS: behaves exactly like v8.4.1; boot shows **no** `[UartLink]` line and Serial1 is never opened.

### Group A — 4-radio mesh routing  *(your Test A — A1/A2 GATING; A3/A4 recommended)*

Setup: **R1 = MT public** (906.875), **R2 = MT private** (905.0, distinct PSK), **R3 = MC public**
(910.525), **R4 = MC private** (909.0, distinct key). R3/R4 are on the co-proc.

**A1 — full mesh** (every radio routes to all three others)
- Do: TX from a node on the MT-public channel.
- PASS: the message appears on the MT-private, MC-public and MC-private networks (cross-protocol
  translated MT↔MC); an echoed copy is loop-dropped (`evt=DROP … drop=…dup`). Repeat sourcing from
  each of the four channels.

**A2 — restricted matrix** (R1↔R2 and R3↔R4 only)
- Do: re-save the matrix so R1 routes only to R2 and R3 only to R4 (and vice-versa); TX from MT-public,
  then from MC-public.
- PASS: MT-public reaches **only** MT-private; MC-public reaches **only** MC-private. Nothing crosses
  between the MT pair and the MC pair → proves `routeMask` isolation.

**A3 — cross-board hop** *(recommended)*
- Do: with R1→R3 routed, TX on R1 (MT).
- PASS: it **egresses on R3** (the co-proc transmits; an MC-public node or the sniffer hears it) —
  proves the UART link carries a TX to a remote radio.

**A4 — loop / dedup at scale** *(recommended)*
- Do: under the full mesh (A1), flood the same packet repeatedly.
- PASS: dedup holds across the 4-way mesh — no runaway re-bridging; duplicates dropped.

### Group B — LoRaWAN + downlink-readiness  *(your Test B + the OTAA-enabler tests)*

Setup: **R1 = MT public**, **R2 = MC public**, **R3 = LoRaWAN Device 1** (DevAddr `0x01000001`,
903.9/SF7), **R4 = LoRaWAN Device 2** (distinct DevAddr/keys, 904.1/SF7).

**B1 — uplink encode + decode** *(GATING)*
- Do: route an MT or MC message to R3 (LoRaWAN); run the sniffer (`bench_lw_sniffer`) on 903.9/SF7.
- PASS: sniffer logs `evt=RX proto=LW devaddr=01000001 …` + `evt=LWRAW raw=…`; feed the raw frame +
  keys to `tools/lw-verify.py` → **MIC PASS** and the decrypted FRMPayload equals the message.

**B2 — two distinct devices** *(recommended)*
- Do: route a message to R3, then one to R4.
- PASS: R3 mints a **DevAddr-1** uplink and R4 a **DevAddr-2** uplink; each decodes with its own keys
  (the per-source ABP device table works across the link, on a remote radio).

**B3 — downlink-listener (the OTAA enabler)** *(recommended — strongly suggested before tag)*
- Setup: R3 = LoRaWAN uplink 903.9/SF7; **R4 = LoRaWAN US915 RX2 = 923.3 / BW500 / SF12**.
- Do: with the sniffer (or a node) transmitting a LoRaWAN frame on 923.3/SF12, also exercise R3 on its
  own channel.
- PASS: **R4 receives the 923.3 frame** (`evt=RX radio=R4 proto=LW`) **while R3 stays on its own
  channel** and remains functional. This proves the bridge can hold a **dedicated always-on downlink
  listener** concurrently with the uplink radio — the exact capability the 4-radio architecture adds for
  v8.5+ OTAA (see §5).

**B4 — intended production topology** *(recommended)*
- Setup: **Board A = mesh** (R1 MT public / R2 MC public); **Board B = LoRaWAN** (R3 uplink 903.9/SF7,
  **R4 = RX2 listener 923.3/SF12**).
- Do: a mesh message → routed to R3 (uplink); independently, a 923.3 frame arrives.
- PASS: the mesh message mints an uplink on R3 (sniffer decodes) **and** R4 hears the 923.3 frame —
  the "mesh board + LoRaWAN board" split runs end-to-end.

### Group C — Robustness  *(recommended / optional)*

**C1 — link resilience** *(recommended)*
- Do: power-cycle the co-processor while the host runs.
- PASS: the host does not crash or wedge; R3/R4 go silent, then recover (`[UartLink] co-processor
  READY` again + `R3/R4 cfg ok`) when the co-proc reboots.

**C2 — sustained load / airtime** *(optional)*
- Do: steady MT/MC traffic under the A1 full mesh.
- PASS: no UART overflow or route-queue starvation; the airtime throttle paces the bridge's TX.

---

## 4. Gating set for the v8.5 tag

**Must pass:** 0.1, 0.2, 0.3, A1, A2, B1.
**Strongly recommended before tag:** A3, B3 (downlink-listener), C1.
**Optional / can defer:** A4, B2, B4, C2.

---

## 5. What this de-risks for v8.5+ OTAA (and what stays future work)

The motivation for going 2→4 radios is **LoRaWAN OTAA**: the device must receive a **JoinAccept
downlink** in an RX window ~1–2 s after its uplink. A 2-radio bridge can't transmit the uplink *and*
retune to listen for the downlink. **Four radios let you dedicate one radio as an always-on downlink
listener** on the fixed US915 RX2 channel (923.3 / SF12) while another radio does the uplink — no
retune.

- ✅ **B3 / B4 prove the radio capability** OTAA needs: a downlink-channel listener running
  *concurrently* with the uplink radio on the co-processor.
- ⏳ **Still future (v8.5+):** the *protocol* layer — opening the RX1/RX2 window timed to an uplink,
  decoding a real JoinAccept, FCnt/DevAddr correlation, and a real LNS-originated downlink (needs a
  downlink-capable gateway behind ChirpStack). This bench confirms the hardware/firmware substrate is
  ready; it does not test the OTAA protocol itself.

---

## 6. Quick reference — serial lines to look for

| Event | Line (host monitor) |
|---|---|
| Co-proc link up | `[UartLink] co-processor READY (… B info)` |
| Co-proc radio configured | `[coproc] R3 cfg ok: 906.875 MHz BW250.0 SF11 CR4/5 20dBm sync0x2B` |
| Host pushed R3/R4 config | `[R3] remote begin -> sent  906.875 MHz BW250.0 SF11 …` |
| A radio received a packet | `… evt=RX radio=R3 …` |
| LoRaWAN capture (sniffer) | `evt=RX proto=LW devaddr=01000001 fcnt=… fport=…` + `evt=LWRAW raw=…` |
| Loop/dup dropped | `… evt=DROP … drop=…dup` |
| Single-board (do-no-harm) | **no** `[UartLink]` line at boot |
