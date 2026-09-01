// esp-otp companion app (Android). Talks the management protocol over BLE:
// manage TOTP tokens, set the clock, and run a background clock-sync service.
import 'package:flutter/material.dart';
import 'package:flutter_foreground_task/flutter_foreground_task.dart';

import 'sync/sync_task.dart';
import 'ui/app_state.dart';
import 'ui/home_page.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  // Required so the app and the foreground-service isolate can exchange data.
  FlutterForegroundTask.initCommunicationPort();
  initForegroundTask();
  runApp(const EspOtpApp());
}

class EspOtpApp extends StatefulWidget {
  const EspOtpApp({super.key});

  @override
  State<EspOtpApp> createState() => _EspOtpAppState();
}

class _EspOtpAppState extends State<EspOtpApp> {
  final AppState _state = AppState();

  @override
  void initState() {
    super.initState();
    _state.init();
  }

  @override
  void dispose() {
    _state.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'esp-otp',
      theme: ThemeData(
        colorSchemeSeed: const Color(0xFF2E7D32),
        useMaterial3: true,
        brightness: Brightness.light,
      ),
      darkTheme: ThemeData(
        colorSchemeSeed: const Color(0xFF2E7D32),
        useMaterial3: true,
        brightness: Brightness.dark,
      ),
      // Wrap so the OS reclaims nothing while the foreground service runs.
      home: WithForegroundTask(child: HomePage(state: _state)),
    );
  }
}
