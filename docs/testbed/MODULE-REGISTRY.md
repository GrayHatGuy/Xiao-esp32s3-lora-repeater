# LR1121 Module Registry

Physical Wio-LR1121 modules are identical parts and indistinguishable in
software by behavior alone. Each LR1121 has a unique 64-bit **chip EUI**
(`GetChipEui`, RadioLib `getChipEui`). `WioLR1121::begin()` prints it at boot:

```
[R2] chip EUI = AA:BB:CC:DD:EE:FF:00:11 (correlate w/ MODULE-REGISTRY.md)
```

Record every module's EUI here so no experiment is ever ambiguous about which
silicon produced a result. **Physical mark:** suspect-BAD carries a red sharpie dot.

## Registry

| EUI | Sharpie mark | Label | History | Status |
|---|---|---|---|---|
| _TBD — read from boot log_ | red dot | suspect-BAD | original module; `state=-20` SPI cascade under sub-GHz TX stress (35 s / 193 s / ~200 s / 408 s, both bands) | suspect silicon-damaged |
| `00:16:C0:01:F0:9B:37:D5` | none | suspect-GOOD | fresh swap-in; clean boot, never cascaded; **currently loaded on bench**. Sub-GHz RX path CONFIRMED FUNCTIONAL 2026-06-01: `[R2 RX] 57 B RSSI -51 SNR 10.2`, decoded Meshtastic, isr fired (DIO9 OK). Caveat: caught was bench-loopback of bridge's own R1 TX at close range — proves RX path, not weak-signal sensitivity. Base FW 1.3; `getErrors=0x0020` (HF_XOSC_START_ERR) present but benign w/ working RX. | suspect good — RX path OK, sensitivity TBD |
| _TBD (on order, 2-10 days)_ | — | pristine-A | unused; cumulative-damage control for `{0,0}` fix | not yet received |
| _TBD (on order, 2-10 days)_ | — | pristine-B | unused; spare / second control | not yet received |

## How to populate

1. Boot the bench with a module mounted; capture the `[R2] chip EUI = ...` line.
2. Note the sharpie mark on the physically-mounted module.
3. Fill the matching row's EUI cell. Repeat per module.
4. Thereafter, the boot-log EUI alone identifies the module in any result.

> First read pending: suspect-GOOD is loaded now — its EUI prints on next boot
> of the EUI-logging firmware.
