// The set of esp-otp devices the app has connected to, remembered so the
// background sync can autoConnect-arm them without scanning. Persisted to
// SharedPreferences (read by both the UI and the foreground-service isolate).
import 'package:shared_preferences/shared_preferences.dart';

const String _kKnown = 'known_devices';

Future<List<String>> loadKnownDevices() async {
  final p = await SharedPreferences.getInstance();
  return p.getStringList(_kKnown) ?? const <String>[];
}

Future<void> addKnownDevice(String id) async {
  final p = await SharedPreferences.getInstance();
  final set = {...(p.getStringList(_kKnown) ?? const <String>[]), id};
  await p.setStringList(_kKnown, set.toList());
}

Future<void> removeKnownDevice(String id) async {
  final p = await SharedPreferences.getInstance();
  final set = {...(p.getStringList(_kKnown) ?? const <String>[])}..remove(id);
  await p.setStringList(_kKnown, set.toList());
}
