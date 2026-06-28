#!/usr/bin/env python3
"""
cam-c2.py — offline build/verify for the LoRaCam C2 frame (mirrors src/CamC2.cpp).

Frame:  [ver:1=0xC2][type:1][senderId:4 LE][recipientId:4 LE][seq:4 LE][ciphertext][cmac:8]
  - per-peer 16-byte PSK -> macKey = AES-ECB(psk,01 00..), encKey = AES-ECB(psk,02 00..)
  - ciphertext = AES-CTR-like keystream (LoRaWAN A_i, devAddr=senderId, fcnt=seq) of the payload
  - cmac       = AES-CMAC(macKey, [bytes 0 .. end-of-ciphertext)) truncated to the leading 8 bytes
This is the same construction the firmware uses; the crypto helpers are byte-identical to
tools/lw-verify.py (already bench-proven against the ABP encoder).

Requires:  pip install cryptography
Usage:
  python cam-c2.py selftest
  python cam-c2.py verify <frame-hex> <psk-hex32>
  python cam-c2.py gen <psk-hex32> <type> <senderId-hex8> <recipId-hex8> <seq> <payload-hex>
      type: 1=CMD 2=ACK 3=EVT 4=BEACON
      e.g. a GET_STATUS (cmd 0x06) command from 0xC0DE to 0xCA00, seq 1:
      python cam-c2.py gen 000102030405060708090a0b0c0d0e0f 1 0000C0DE 0000CA00 1 06
"""
import sys

try:
    from cryptography.hazmat.primitives.cmac import CMAC
    from cryptography.hazmat.primitives.ciphers import Cipher, modes
    from cryptography.hazmat.primitives.ciphers.algorithms import AES
except ImportError:
    sys.exit("error: pip install cryptography")

MAGIC = 0xC2
HDR = 14
TAG = 8
TYPES = {1: "CMD", 2: "ACK", 3: "EVT", 4: "BEACON"}


def aes_ecb(key: bytes, block: bytes) -> bytes:
    e = Cipher(AES(key), modes.ECB()).encryptor()
    return e.update(block) + e.finalize()


def aes_cmac(key: bytes, data: bytes) -> bytes:
    c = CMAC(AES(key))
    c.update(data)
    return c.finalize()


def crypt_frm(key: bytes, dev_addr: int, fcnt32: int, payload: bytes) -> bytes:
    """LoRaWAN-style CTR keystream, dir=0 uplink (mirrors LoRaWANCrypto::cryptFrmPayload)."""
    if not payload:
        return payload
    out = bytearray(payload)
    blocks = (len(payload) + 15) // 16
    for i in range(1, blocks + 1):
        a = bytes([
            0x01, 0, 0, 0, 0, 0,
            dev_addr & 0xFF, (dev_addr >> 8) & 0xFF, (dev_addr >> 16) & 0xFF, (dev_addr >> 24) & 0xFF,
            fcnt32 & 0xFF, (fcnt32 >> 8) & 0xFF, (fcnt32 >> 16) & 0xFF, (fcnt32 >> 24) & 0xFF,
            0x00, i & 0xFF,
        ])
        s = aes_ecb(key, a)
        off = (i - 1) * 16
        for j in range(min(16, len(payload) - off)):
            out[off + j] ^= s[j]
    return bytes(out)


def derive_keys(psk: bytes):
    """(macKey, encKey) — domain-separated from the PSK via AES-ECB labels 0x01 / 0x02."""
    mac_key = aes_ecb(psk, bytes([0x01] + [0] * 15))
    enc_key = aes_ecb(psk, bytes([0x02] + [0] * 15))
    return mac_key, enc_key


def _le32(v: int) -> bytes:
    return bytes([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF])


def _rd32(b: bytes) -> int:
    return b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)


def build(psk: bytes, typ: int, sender: int, recip: int, seq: int, payload: bytes) -> bytes:
    mac_key, enc_key = derive_keys(psk)
    ct = crypt_frm(enc_key, sender, seq, payload)             # encrypt-then-MAC
    body = bytes([MAGIC, typ]) + _le32(sender) + _le32(recip) + _le32(seq) + ct
    tag = aes_cmac(mac_key, body)[:TAG]
    return body + tag


def verify(frame: bytes, psk: bytes):
    if len(frame) < HDR + TAG:
        return False, "too short", None
    if frame[0] != MAGIC:
        return False, "bad magic (not 0xC2)", None
    mac_key, enc_key = derive_keys(psk)
    authlen = len(frame) - TAG
    calc = aes_cmac(mac_key, frame[:authlen])[:TAG]
    if calc != frame[authlen:]:
        return False, "MIC FAIL (wrong key or forged/tampered)", None
    typ, sender, recip, seq = frame[1], _rd32(frame[2:6]), _rd32(frame[6:10]), _rd32(frame[10:14])
    pt = crypt_frm(enc_key, sender, seq, frame[HDR:authlen])
    info = dict(type=typ, type_name=TYPES.get(typ, "?"),
                sender=sender, recip=recip, seq=seq, payload=pt)
    return True, "MIC PASS", info


def selftest() -> bool:
    ok = True
    # 1) RFC 4493 AES-CMAC KAT (len=16) — the SAME vector LoRaWANCrypto.h::selfTest uses,
    #    so a PASS here means this tool's CMAC matches the firmware's hand-rolled CMAC.
    K = bytes.fromhex("2b7e151628aed2a6abf7158809cf4f3c")
    M = bytes.fromhex("6bc1bee22e409f96e93d7e117393172a")
    EXP = bytes.fromhex("070a16b46b4d4144f79bdd9dd04a287c")
    p = aes_cmac(K, M) == EXP
    ok &= p
    print(f"[selftest] RFC4493 CMAC len=16 : {'PASS' if p else 'FAIL'}")
    # 2) build -> verify round-trip recovers the plaintext (mirrors CamC2 camSelfTest inputs)
    psk = bytes(range(1, 17))
    pt = bytes([0x06, 0xAA, 0xBB])           # CMD_GET_STATUS + args
    fr = build(psk, 1, 0x11223344, 0x55667788, 7, pt)
    okv, _, info = verify(fr, psk)
    p = okv and info["payload"] == pt
    ok &= p
    print(f"[selftest] roundtrip : {'PASS' if p else 'FAIL'}")
    # 3) a one-bit tag flip must be rejected
    bad = bytearray(fr)
    bad[-1] ^= 0x01
    okv2, _, _ = verify(bytes(bad), psk)
    p = not okv2
    ok &= p
    print(f"[selftest] tamper-reject : {'PASS' if p else 'FAIL'}")
    print(f"[selftest] overall : {'PASS' if ok else 'FAIL'}")
    return ok


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    cmd = sys.argv[1]
    if cmd == "selftest":
        sys.exit(0 if selftest() else 1)
    if cmd == "verify":
        frame = bytes.fromhex(sys.argv[2].replace(" ", ""))
        psk = bytes.fromhex(sys.argv[3])
        okv, msg, info = verify(frame, psk)
        print(msg)
        if info:
            print(f"  type     = {info['type']} ({info['type_name']})")
            print(f"  senderId = 0x{info['sender']:08X}")
            print(f"  recipId  = 0x{info['recip']:08X}" + ("  (broadcast)" if info['recip'] == 0 else ""))
            print(f"  seq      = {info['seq']}")
            print(f"  payload  = {info['payload'].hex()}")
        sys.exit(0 if okv else 1)
    if cmd == "gen":
        psk = bytes.fromhex(sys.argv[2])
        typ = int(sys.argv[3])
        sender = int(sys.argv[4], 16)
        recip = int(sys.argv[5], 16)
        seq = int(sys.argv[6])
        payload = bytes.fromhex(sys.argv[7]) if len(sys.argv) > 7 else b""
        print(build(psk, typ, sender, recip, seq, payload).hex())
        sys.exit(0)
    print(__doc__)
    sys.exit(2)


if __name__ == "__main__":
    main()
