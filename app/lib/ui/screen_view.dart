// Live mirror of the device OLED: polls GET_SCREEN and paints the column-major
// framebuffer (each byte = 8 vertical pixels, bit0 top) — like the web console.
import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/material.dart';

import '../ble/otp_device.dart';

class ScreenView extends StatefulWidget {
  final OtpDevice device;
  const ScreenView({super.key, required this.device});

  @override
  State<ScreenView> createState() => _ScreenViewState();
}

class _ScreenViewState extends State<ScreenView> {
  Timer? _timer;
  bool _busy = false;
  int _w = 72, _pages = 5;
  Uint8List _fb = Uint8List(0);
  String? _err;

  @override
  void initState() {
    super.initState();
    _poll();
    _timer = Timer.periodic(const Duration(seconds: 1), (_) => _poll());
  }

  Future<void> _poll() async {
    // Skip while the device is paused (e.g. during OS bonding) or not yet secure
    // (unbonded) so the 1 Hz poll doesn't flood the link and time out the SMP
    // encryption handshake — and doesn't hammer a link the firmware will reject.
    if (_busy || widget.device.paused || !widget.device.secure) return;
    _busy = true;
    try {
      final s = await widget.device.getScreen();
      if (!mounted) return;
      setState(() {
        _w = s.width;
        _pages = s.pages;
        _fb = s.fb;
        _err = null;
      });
    } catch (e) {
      if (mounted && _err == null) setState(() => _err = '$e');
    } finally {
      _busy = false;
    }
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (_err != null) {
      return Text('screen unavailable: $_err',
          style: const TextStyle(color: Colors.orange));
    }
    return AspectRatio(
      aspectRatio: _w / (_pages * 8),
      child: Container(
        decoration: BoxDecoration(
          color: const Color(0xFF040c08),
          borderRadius: BorderRadius.circular(6),
          border: Border.all(color: Colors.black54),
        ),
        child: CustomPaint(painter: _FbPainter(_w, _pages, _fb)),
      ),
    );
  }
}

class _FbPainter extends CustomPainter {
  final int w, pages;
  final Uint8List fb;
  _FbPainter(this.w, this.pages, this.fb);

  @override
  void paint(Canvas canvas, Size size) {
    if (fb.isEmpty) return;
    final h = pages * 8;
    final sx = size.width / w, sy = size.height / h;
    final on = Paint()..color = const Color(0xFF39FF8C);
    for (int page = 0; page < pages; page++) {
      for (int col = 0; col < w; col++) {
        final idx = page * w + col;
        if (idx >= fb.length) continue;
        final byte = fb[idx];
        for (int bit = 0; bit < 8; bit++) {
          if ((byte >> bit) & 1 == 1) {
            final y = page * 8 + bit;
            canvas.drawRect(
                Rect.fromLTWH(col * sx, y * sy, sx + 0.5, sy + 0.5), on);
          }
        }
      }
    }
  }

  @override
  bool shouldRepaint(_FbPainter old) => old.fb != fb;
}
