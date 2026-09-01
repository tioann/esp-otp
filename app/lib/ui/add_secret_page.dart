// Add a TOTP secret: paste an otpauth:// URI (auto-fills the form) or enter a
// label + base32 secret with digits/period/algorithm. Mirrors host_cli `add`.
import 'package:flutter/material.dart';

import '../proto/otpauth.dart';
import '../proto/protocol.dart';

class AddSecretPage extends StatefulWidget {
  const AddSecretPage({super.key});

  @override
  State<AddSecretPage> createState() => _AddSecretPageState();
}

class _AddSecretPageState extends State<AddSecretPage> {
  final _label = TextEditingController();
  final _secret = TextEditingController();
  final _uri = TextEditingController();
  int _digits = 6;
  int _period = 30;
  int _algo = 0;
  String? _error;

  void _applyUri() {
    try {
      final s = parseOtpauth(_uri.text);
      setState(() {
        _label.text = s.label.length > 31 ? s.label.substring(0, 31) : s.label;
        _secret.text = s.secret;
        _digits = s.digits;
        _period = s.period;
        _algo = s.algo;
        _error = null;
      });
    } catch (e) {
      setState(() => _error = '$e');
    }
  }

  void _submit() {
    final label = _label.text.trim();
    final secret = _secret.text.replaceAll(RegExp(r'\s+'), '').toUpperCase();
    if (label.isEmpty) {
      setState(() => _error = 'label required');
      return;
    }
    try {
      checkBase32(secret);
    } catch (e) {
      setState(() => _error = '$e');
      return;
    }
    Navigator.pop(
      context,
      OtpSpec(
          label: label,
          secret: secret,
          digits: _digits,
          period: _period,
          algo: _algo),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Add token')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          Text('Paste an otpauth:// URI',
              style: Theme.of(context).textTheme.titleSmall),
          const SizedBox(height: 8),
          Row(children: [
            Expanded(
              child: TextField(
                controller: _uri,
                decoration: const InputDecoration(
                  hintText: 'otpauth://totp/…',
                  border: OutlineInputBorder(),
                  isDense: true,
                ),
              ),
            ),
            const SizedBox(width: 8),
            FilledButton(onPressed: _applyUri, child: const Text('Parse')),
          ]),
          const Divider(height: 32),
          TextField(
            controller: _label,
            maxLength: 31,
            decoration: const InputDecoration(
              labelText: 'Label',
              border: OutlineInputBorder(),
            ),
          ),
          const SizedBox(height: 8),
          TextField(
            controller: _secret,
            decoration: const InputDecoration(
              labelText: 'Base32 secret',
              border: OutlineInputBorder(),
            ),
          ),
          const SizedBox(height: 16),
          Row(children: [
            Expanded(
              child: _numField('Digits', _digits, (v) => setState(() => _digits = v)),
            ),
            const SizedBox(width: 12),
            Expanded(
              child: _numField('Period (s)', _period,
                  (v) => setState(() => _period = v)),
            ),
          ]),
          const SizedBox(height: 16),
          DropdownButtonFormField<int>(
            value: _algo,
            decoration: const InputDecoration(
              labelText: 'Algorithm',
              border: OutlineInputBorder(),
            ),
            items: [
              for (int i = 0; i < algoNames.length; i++)
                DropdownMenuItem(value: i, child: Text(algoNames[i])),
            ],
            onChanged: (v) => setState(() => _algo = v ?? 0),
          ),
          if (_error != null) ...[
            const SizedBox(height: 16),
            Text(_error!, style: const TextStyle(color: Colors.red)),
          ],
          const SizedBox(height: 24),
          FilledButton.icon(
            onPressed: _submit,
            icon: const Icon(Icons.add),
            label: const Text('Add to device'),
          ),
        ],
      ),
    );
  }

  Widget _numField(String label, int value, ValueChanged<int> onChanged) {
    return TextFormField(
      initialValue: '$value',
      keyboardType: TextInputType.number,
      decoration: InputDecoration(
        labelText: label,
        border: const OutlineInputBorder(),
      ),
      onChanged: (s) {
        final v = int.tryParse(s);
        if (v != null) onChanged(v);
      },
    );
  }
}
