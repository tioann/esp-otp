// Transport abstraction: the device client only needs to push a framed packet
// out and receive raw response bytes in. BLE and (future) serial both implement
// this, so proto/OtpDevice logic is transport-agnostic — same split as the CLI.
import 'dart:typed_data';

abstract class Transport {
  /// Raw bytes as they arrive on the Response characteristic (one notification
  /// may carry part of, one, or several frames).
  Stream<List<int>> get incoming;

  /// Write one full framed packet to the Command characteristic.
  Future<void> send(Uint8List frame);

  Future<void> dispose();
}
