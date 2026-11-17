#!/usr/bin/env python3
"""Prints the current TOTP code (RFC 6238: SHA-1, 6 digits, 30 s) of the
2FA test server's secret, or of the secret given as the first argument.

    tests/totp.py [BASE32SECRET] [OFFSET_SECONDS]
"""
import base64
import hashlib
import hmac
import struct
import sys
import time

SECRET = "JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP"  # testserver/otp/google_authenticator


def totp(secret=SECRET, at=None):
    key = base64.b32decode(secret.upper() + "=" * (-len(secret) % 8))
    counter = int((time.time() if at is None else at) // 30)
    mac = hmac.new(key, struct.pack(">Q", counter), hashlib.sha1).digest()
    off = mac[-1] & 0x0F
    code = (struct.unpack(">I", mac[off:off + 4])[0] & 0x7FFFFFFF) % 1000000
    return f"{code:06d}"


if __name__ == "__main__":
    secret = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1] != "-" else SECRET
    offset = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    print(totp(secret, time.time() + offset))
