// Scan / connect landing screen. Lists esp-otp devices in range, a Connect
// action, and the background clock-sync toggle. When connected it shows the
// device page instead.
import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import 'app_state.dart';
import 'device_page.dart';

class HomePage extends StatelessWidget {
  final AppState state;
  const HomePage({super.key, required this.state});

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: state,
      builder: (context, _) {
        if (state.conn == ConnState.connected) {
          return DevicePage(state: state);
        }
        return _scanScaffold(context);
      },
    );
  }

  Widget _scanScaffold(BuildContext context) {
    final btOff = state.adapterState != BluetoothAdapterState.on &&
        state.adapterState != BluetoothAdapterState.unknown;
    return Scaffold(
      appBar: AppBar(
        title: const Text('esp-otp'),
        actions: [
          IconButton(
            tooltip: state.scanning ? 'Stop scan' : 'Scan',
            onPressed: state.scanning ? state.stopScanning : state.startScan,
            icon: Icon(state.scanning ? Icons.stop : Icons.search),
          ),
        ],
      ),
      body: Column(
        children: [
          _syncTile(context),
          const Divider(height: 1),
          if (btOff)
            const Padding(
              padding: EdgeInsets.all(16),
              child: Text('Bluetooth is off — enable it to scan.',
                  style: TextStyle(color: Colors.orange)),
            ),
          if (state.lastError != null)
            Padding(
              padding: const EdgeInsets.all(16),
              child: Text(state.lastError!,
                  style: const TextStyle(color: Colors.red)),
            ),
          if (state.conn == ConnState.connecting)
            const LinearProgressIndicator(),
          Expanded(child: _deviceList(context)),
        ],
      ),
      floatingActionButton: FloatingActionButton.extended(
        onPressed: state.scanning ? state.stopScanning : state.startScan,
        icon: Icon(state.scanning ? Icons.stop : Icons.search),
        label: Text(state.scanning ? 'Scanning…' : 'Scan'),
      ),
    );
  }

  Widget _syncTile(BuildContext context) {
    return SwitchListTile(
      secondary: const Icon(Icons.sync),
      title: const Text('Background clock sync'),
      subtitle: const Text(
          'Keep a foreground service that time-syncs any esp-otp on connect'),
      value: state.syncServiceRunning,
      onChanged: (v) => state.setBackgroundSync(v),
    );
  }

  Widget _deviceList(BuildContext context) {
    final results = state.scanResults;
    if (results.isEmpty) {
      return Center(
        child: Text(state.scanning ? 'Scanning…' : 'No devices — tap Scan'),
      );
    }
    return ListView.separated(
      itemCount: results.length,
      separatorBuilder: (_, __) => const Divider(height: 1),
      itemBuilder: (context, i) {
        final r = results[i];
        // Prefer the freshly ADVERTISED name (scan-response) over
        // device.platformName: on Android platformName is the OS's cached GAP
        // name, which is sticky (especially once bonded) and does NOT update
        // when the device is renamed. advName reflects the live advertisement,
        // so a rename shows up immediately.
        final name = r.advertisementData.advName.isNotEmpty
            ? r.advertisementData.advName
            : (r.device.platformName.isNotEmpty
                ? r.device.platformName
                : 'esp-otp');
        return ListTile(
          leading: const Icon(Icons.security),
          title: Text(name),
          subtitle: Text('${r.device.remoteId.str} · ${r.rssi} dBm'),
          trailing: FilledButton(
            onPressed: () => state.connect(r.device),
            child: const Text('Connect'),
          ),
        );
      },
    );
  }
}
