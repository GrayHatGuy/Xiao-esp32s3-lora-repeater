# Changelog

## 2026-05-18 — RNS fragmentation

RNS bridging now uses base64 + auto-fragmentation across MT/MC packets with `[rns <seq> <x>/<y>]` markers, CRC-16 sequence IDs, per-protocol fragment pacing, and 8-fragment cap.
