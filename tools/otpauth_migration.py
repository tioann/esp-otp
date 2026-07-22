#--!/usr/bin/env python3
"""Convert a Google Authenticator export (otpauth-migration://) into plain
otpauth:// TOTP URLs.

Google Authenticator's "export accounts" QR encodes an
`otpauth-migration://offline?data=<base64 protobuf>` URI that bundles many
accounts with *raw* secrets. This tool decodes it (no protobuf dependency: the
wire format is parsed by hand) and prints one standard `otpauth://totp/...` URL
per TOTP account to stdout, ready to feed to host_cli.py:

    tools/otpauth_migration.py "otpauth-migration://offline?data=..." \\
        | while read url; do tools/host_cli.py -p /dev/ttyACM0 add "$url"; done

The migration URI can be passed as arguments or on stdin (one per line). HOTP
and MD5 accounts are skipped (the device does TOTP with SHA1/256/512 only).

    tools/otpauth_migration.py --selftest      # decode a known vector, no input
"""
import argparse
import base64
import sys
from urllib.parse import urlparse, parse_qs, quote, urlencode

# MigrationPayload enums (0 = invalid in each).
_ALGO_NAME = {1: "SHA1", 2: "SHA256", 3: "SHA512"}   # 4 = MD5 (unsupported)
_DIGITS = {1: 6, 2: 8}                                # SIX / EIGHT
_OTP_TOTP = 2                                         # OtpType.OTP_TYPE_TOTP


def _read_varint(buf, i):
    result = shift = 0
    while True:
        if i >= len(buf):
            raise ValueError("truncated varint")
        b = buf[i]
        i += 1
        result |= (b & 0x7F) << shift
        if not (b & 0x80):
            return result, i
        shift += 7


def _fields(buf):
    """Yield (field_number, wire_type, value) for a protobuf message; `value` is
    an int for varints and a bytes slice for length-delimited fields."""
    i, n = 0, len(buf)
    while i < n:
        tag, i = _read_varint(buf, i)
        field, wire = tag >> 3, tag & 7
        if wire == 0:
            val, i = _read_varint(buf, i)
            yield field, wire, val
        elif wire == 2:
            ln, i = _read_varint(buf, i)
            yield field, wire, buf[i:i + ln]
            i += ln
        elif wire == 5:
            yield field, wire, buf[i:i + 4]; i += 4
        elif wire == 1:
            yield field, wire, buf[i:i + 8]; i += 8
        else:
            raise ValueError(f"unsupported protobuf wire type {wire}")


def _otp_param_to_url(msg):
    """One OtpParameters submessage -> otpauth:// URL, or None if unsupported
    (HOTP, MD5, or missing secret)."""
    secret = b""
    name = issuer = ""
    algo_enum = digits_enum = otype = 0
    for field, wire, val in _fields(msg):
        if field == 1 and wire == 2:
            secret = val
        elif field == 2 and wire == 2:
            name = val.decode("utf-8", "replace")
        elif field == 3 and wire == 2:
            issuer = val.decode("utf-8", "replace")
        elif field == 4 and wire == 0:
            algo_enum = val
        elif field == 5 and wire == 0:
            digits_enum = val
        elif field == 6 and wire == 0:
            otype = val
        # field 7 (HOTP counter) ignored
    if otype and otype != _OTP_TOTP:
        return None  # skip HOTP
    if algo_enum and algo_enum not in _ALGO_NAME:
        return None  # skip MD5 / unknown
    if not secret:
        return None

    label = name or issuer
    # Raw secret bytes -> base32 (unpadded), the form otpauth:// expects.
    b32 = base64.b32encode(secret).decode("ascii").rstrip("=")
    query = {
        "secret": b32,
        "algorithm": _ALGO_NAME.get(algo_enum, "SHA1"),
        "digits": _DIGITS.get(digits_enum, 6),
        "period": 30,  # migration payloads carry no period; GA is always 30
    }
    if issuer:
        query["issuer"] = issuer
    return f"otpauth://totp/{quote(label, safe='')}?{urlencode(query)}"


def migration_to_urls(uri):
    """otpauth-migration://offline?data=... -> list of otpauth:// URLs (TOTP
    accounts only)."""
    u = urlparse(uri)
    if u.scheme != "otpauth-migration":
        raise ValueError(f"not an otpauth-migration:// URI: {uri[:40]!r}")
    data = parse_qs(u.query).get("data", [""])[0]
    if not data:
        raise ValueError("migration URI has no data")
    try:
        raw = base64.b64decode(data + "=" * (-len(data) % 4), validate=True)
    except ValueError as e:  # binascii.Error subclasses ValueError
        raise ValueError(f"migration data is not valid base64: {e}")

    urls = []
    for field, wire, val in _fields(raw):
        if field == 1 and wire == 2:  # repeated otp_parameters submessage
            url = _otp_param_to_url(val)
            if url is not None:
                urls.append(url)
    return urls


def selftest():
    # Canonical Google Authenticator export vector: one account, secret bytes
    # "Hello!\xde\xad\xbe\xef" -> base32 JBSWY3DPEHPK3PXP.
    urls = migration_to_urls(
        "otpauth-migration://offline?data=CjEKCkhlbGxvId6tvu8SGEV4YW1wbGU6Y"
        "WxpY2VAZ29vZ2xlLmNvbRoHRXhhbXBsZSABKAEwAhABGAEgAA%3D%3D")
    expect = ("otpauth://totp/Example%3Aalice%40google.com?"
              "secret=JBSWY3DPEHPK3PXP&algorithm=SHA1&digits=6&period=30&issuer=Example")
    assert urls == [expect], urls
    for bad in ("otpauth://totp/x?secret=A", "otpauth-migration://offline?data="):
        try:
            migration_to_urls(bad)
        except ValueError:
            pass
        else:
            raise AssertionError(f"expected reject: {bad}")
    print("otpauth_migration selftest OK")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("uri", nargs="*",
                    help="otpauth-migration:// URI(s); if none, read them from stdin")
    ap.add_argument("--selftest", action="store_true",
                    help="decode a known vector and exit (no input needed)")
    args = ap.parse_args()

    if args.selftest:
        selftest()
        return

    uris = args.uri or [ln.strip() for ln in sys.stdin if ln.strip()]
    if not uris:
        ap.error("no migration URI given (as an argument or on stdin)")

    total = 0
    for uri in uris:
        try:
            urls = migration_to_urls(uri)
        except ValueError as e:
            sys.exit(f"error: {e}")
        for url in urls:
            print(url)
        total += len(urls)
        print(f"# {len(urls)} TOTP account(s) from this export", file=sys.stderr)
    if total == 0:
        sys.exit("no TOTP accounts found (HOTP/MD5 entries are skipped)")


if __name__ == "__main__":
    main()
