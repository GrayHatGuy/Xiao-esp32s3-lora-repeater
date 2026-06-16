#!/usr/bin/env python3
"""
lw-verify.py — off-box "synthetic LNS" verifier for the v8.4 ABP encoder.

Takes a LoRaWAN 1.0.x uplink PHYPayload (the `evt=LWRAW raw=…` hex printed by the
bench_lw_sniffer build) plus the device's NwkSKey/AppSKey, and:
  - parses the MHDR / DevAddr / FCnt / FPort,
  - verifies the MIC = aes128_cmac(NwkSKey, B0 | msg)[0:4], and
  - decrypts the FRMPayload (AppSKey for FPort>0, NwkSKey for FPort==0).

A PASS here means a real ChirpStack would accept the frame (barring NetID/provisioning),
so you can validate the encoder crypto on the bench WITHOUT a ChirpStack LNS.

Mirrors src/LoRaWANCrypto.h byte-for-byte (B0/A_i blocks, LE DevAddr+FCnt, dir=0 uplink).

Requires:  pip install cryptography
Usage:
  python lw-verify.py <PHYPayload-hex> <NwkSKey-hex32> <AppSKey-hex32> [--fcnt-msb N] [--tagged]

Example (the bench creds + a frame copied from `evt=LWRAW raw=…`):
  python lw-verify.py 40011000... 2B7E151628AED2A6ABF7158809CF4F3C D41420B7F5A3C96E1D8204F7B3A65C90
"""
import argparse
import sys

try:
    from cryptography.hazmat.primitives.cmac import CMAC
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    from cryptography.hazmat.primitives.ciphers.algorithms import AES
except ImportError:
    sys.exit("error: pip install cryptography")


def aes_ecb(key: bytes, block: bytes) -> bytes:
    enc = Cipher(AES(key), modes.ECB()).encryptor()
    return enc.update(block) + enc.finalize()


def aes_cmac(key: bytes, data: bytes) -> bytes:
    c = CMAC(AES(key))
    c.update(data)
    return c.finalize()


def crypt_frmpayload(key: bytes, dev_addr: int, fcnt32: int, payload: bytes) -> bytes:
    out = bytearray(payload)
    blocks = (len(payload) + 15) // 16
    for i in range(1, blocks + 1):
        a = bytes([0x01, 0,0,0,0, 0x00,                      # 0x01 | 4x00 | dir=0
                   dev_addr & 0xFF, (dev_addr >> 8) & 0xFF,
                   (dev_addr >> 16) & 0xFF, (dev_addr >> 24) & 0xFF,
                   fcnt32 & 0xFF, (fcnt32 >> 8) & 0xFF,
                   (fcnt32 >> 16) & 0xFF, (fcnt32 >> 24) & 0xFF,
                   0x00, i & 0xFF])
        s = aes_ecb(key, a)
        off = (i - 1) * 16
        for j in range(min(16, len(payload) - off)):
            out[off + j] ^= s[j]
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="Offline LoRaWAN 1.0.x ABP uplink MIC + decrypt verifier")
    ap.add_argument("phypayload", help="full PHYPayload hex (from evt=LWRAW raw=…)")
    ap.add_argument("nwkskey", help="NwkSKey, 32 hex chars")
    ap.add_argument("appskey", help="AppSKey, 32 hex chars")
    ap.add_argument("--fcnt-msb", type=int, default=0,
                    help="upper 16 bits of FCnt (default 0; raise once the LNS counter passes 65535)")
    ap.add_argument("--tagged", action="store_true",
                    help="interpret the first 5 decrypted bytes as [proto:1][srcId:4 LE]")
    a = ap.parse_args()

    phy = bytes.fromhex(a.phypayload.replace(" ", ""))
    nwk = bytes.fromhex(a.nwkskey)
    app = bytes.fromhex(a.appskey)
    if len(nwk) != 16 or len(app) != 16:
        return print("error: keys must be 32 hex chars (16 bytes)") or 2
    if len(phy) < 12:
        return print("error: PHYPayload too short for a data frame") or 2

    mhdr = phy[0]
    mtype = mhdr >> 5
    dev_addr = phy[1] | (phy[2] << 8) | (phy[3] << 16) | (phy[4] << 24)
    fctrl = phy[5]
    fcnt16 = phy[6] | (phy[7] << 8)
    fopts_len = fctrl & 0x0F
    # FPort index = MHDR(1) + DevAddr(4) + FCtrl(1) + FCnt(2) + FOpts(fopts_len) = 8 + fopts_len
    fport_idx = 8 + fopts_len
    msg = phy[:-4]            # PHYPayload minus the 4-byte MIC
    mic = phy[-4:]

    has_fport = len(msg) > fport_idx
    fport = msg[fport_idx] if has_fport else None
    frm = msg[fport_idx + 1:] if has_fport else b""
    fcnt32 = (a.fcnt_msb << 16) | fcnt16

    mtype_name = {0: "JoinReq", 2: "UnconfUp", 4: "ConfUp"}.get(mtype, f"MType{mtype}")
    print(f"MHDR        : 0x{mhdr:02X}  ({mtype_name})")
    print(f"DevAddr     : 0x{dev_addr:08X}  (NwkID {dev_addr >> 25})")
    print(f"FCtrl/FCnt  : 0x{fctrl:02X} / {fcnt16}  (FCnt32={fcnt32}, msb={a.fcnt_msb})")
    print(f"FPort       : {fport}")
    print(f"FRMPayload  : {frm.hex()}  ({len(frm)} B, encrypted)")

    # MIC = aes128_cmac(NwkSKey, B0 | msg)[0:4]
    b0 = bytes([0x49, 0,0,0,0, 0x00,
                dev_addr & 0xFF, (dev_addr >> 8) & 0xFF,
                (dev_addr >> 16) & 0xFF, (dev_addr >> 24) & 0xFF,
                fcnt32 & 0xFF, (fcnt32 >> 8) & 0xFF,
                (fcnt32 >> 16) & 0xFF, (fcnt32 >> 24) & 0xFF,
                0x00, len(msg) & 0xFF])
    calc = aes_cmac(nwk, b0 + msg)[:4]
    mic_ok = calc == mic
    print(f"MIC         : got {mic.hex()}  expect {calc.hex()}  -> {'PASS' if mic_ok else 'FAIL'}")

    if has_fport and frm:
        key = nwk if fport == 0 else app
        pt = crypt_frmpayload(key, dev_addr, fcnt32, frm)
        ascii_view = "".join(chr(c) if 0x20 <= c <= 0x7E else "." for c in pt)
        print(f"Decrypted   : {pt.hex()}")
        print(f"  as ASCII  : {ascii_view!r}")
        if a.tagged and len(pt) >= 5:
            proto = pt[0]
            src_id = pt[1] | (pt[2] << 8) | (pt[3] << 16) | (pt[4] << 24)
            name = {1: "meshtastic", 2: "meshcore", 3: "reticulum", 4: "custom", 5: "lorawan"}.get(proto, "?")
            body = pt[5:]
            print(f"  src tag   : proto={proto}({name}) srcId=0x{src_id:08X}")
            print(f"  payload   : {body.hex()}  {''.join(chr(c) if 0x20 <= c <= 0x7E else '.' for c in body)!r}")

    print(f"\nVERDICT: {'MIC valid — ChirpStack would ACCEPT this frame' if mic_ok else 'MIC INVALID — ChirpStack would silently DROP it'}")
    return 0 if mic_ok else 1


if __name__ == "__main__":
    sys.exit(main())
