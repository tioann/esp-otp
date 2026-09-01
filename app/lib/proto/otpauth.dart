// otpauth:// parsing and base32 validation — twin of host_cli.py's
// parse_otpauth / check_base32. Lets a typo fail before any bytes hit the wire.

const Map<String, int> algoByName = {'SHA1': 0, 'SHA256': 1, 'SHA512': 2};

// RFC 4648 base32 data alphabet (case-insensitive). The device also ignores
// space, '-' and '=' as separators/padding (see totp.c base32_decode).
final Set<String> _b32Alphabet =
    'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz234567'.split('').toSet();
final Set<String> _b32Separators = {' ', '-', '='};

/// Validate a base32 secret the way the device will. Throws [FormatException].
void checkBase32(String secret) {
  final data = secret.split('').where((c) => !_b32Separators.contains(c));
  if (data.isEmpty) throw const FormatException('empty base32 secret');
  for (final c in data) {
    if (!_b32Alphabet.contains(c)) {
      throw FormatException('invalid base32 character "$c" in secret');
    }
  }
}

/// A resolved secret ready to ADD.
class OtpSpec {
  String label;
  String secret;
  int digits;
  int period;
  int algo;
  OtpSpec({
    required this.label,
    required this.secret,
    this.digits = 6,
    this.period = 30,
    this.algo = 0,
  });
}

/// Parse an `otpauth://totp/LABEL?secret=…&algorithm=…&digits=…&period=…` URI.
/// Only TOTP is supported. Throws [FormatException] on anything else.
OtpSpec parseOtpauth(String raw) {
  final u = Uri.parse(raw.trim());
  if (u.scheme != 'otpauth') throw const FormatException('not an otpauth:// URI');
  if (u.host.toLowerCase() != 'totp') {
    throw FormatException('unsupported otpauth type "${u.host}" (only totp)');
  }
  final secret = (u.queryParameters['secret'] ?? '').replaceAll(RegExp(r'\s+'), '');
  if (secret.isEmpty) throw const FormatException('otpauth URI has no secret');

  // Label is the URL-decoded path ("Issuer:Account" or "Account"); fall back to
  // the issuer param when the path is empty.
  String label = Uri.decodeComponent(
      u.path.startsWith('/') ? u.path.substring(1) : u.path).trim();
  if (label.isEmpty) label = (u.queryParameters['issuer'] ?? '').trim();

  final algoName = (u.queryParameters['algorithm'] ?? 'SHA1').toUpperCase();
  final algo = algoByName[algoName];
  if (algo == null) throw FormatException('unsupported algorithm "$algoName"');

  return OtpSpec(
    label: label,
    secret: secret,
    digits: int.tryParse(u.queryParameters['digits'] ?? '6') ?? 6,
    period: int.tryParse(u.queryParameters['period'] ?? '30') ?? 30,
    algo: algo,
  );
}
