import 'dart:io';
import 'package:flutter/material.dart';
import 'package:window_manager/window_manager.dart';
import 'package:flutter/services.dart';


void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await windowManager.ensureInitialized();

  WindowOptions windowOptions = const WindowOptions(
    size: Size(450, 400),
    center: true,
    backgroundColor: Colors.transparent,
    skipTaskbar: false,
    title: "RDMA Remote Control",
  );

  windowManager.waitUntilReadyToShow(windowOptions, () async {
    await windowManager.show();
    await windowManager.focus();
  });

  runApp(const LauncherApp());
}

class LauncherApp extends StatelessWidget {
  const LauncherApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        brightness: Brightness.dark,
        useMaterial3: true,
        colorSchemeSeed: Colors.green,
      ),
      home: const Scaffold(
        body: SafeArea(
          child: Padding(
            padding: EdgeInsets.all(24.0),
            child: LauncherForm(),
          ),
        ),
      ),
    );
  }
}

class LauncherForm extends StatefulWidget {
  const LauncherForm({super.key});

  @override
  State<LauncherForm> createState() => _LauncherFormState();
}

class _LauncherFormState extends State<LauncherForm> {
  static const _channel = MethodChannel('com.app.launcher/control');

  final _formKey = GlobalKey<FormState>();
  final _ipController = TextEditingController();

  bool _isServer = false;
  bool _isTest = false;
  bool _isCompress = false;

  String? _validateIp(String? value) {
    if (value == null || value.trim().isEmpty) {
      return 'Please enter an IP address';
    }
    final parsed = InternetAddress.tryParse(value.trim());
    if (parsed == null || parsed.type != InternetAddressType.IPv4) {
      return 'Please enter a valid IPv4 address';
    }
    return null;
  }

  void _onLaunch() async {
    if (!_formKey.currentState!.validate()) return;

    await _channel.invokeMethod('submitAndExit', {
      'ip': _ipController.text.trim(),
      'mode': _isServer ? 'server' : 'client',
      'isTest': !_isServer && _isTest,
      'isCompress': !_isServer && _isCompress,
    });
  }

  @override
  Widget build(BuildContext context) {
    return Form(
      key: _formKey,
      autovalidateMode: AutovalidateMode.onUserInteraction,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Container(
                width: 100, height: 40,
                padding: const EdgeInsets.only(top: 12),
                child: const Text('Host IP:', style: TextStyle(fontWeight: FontWeight.w500)),
              ),

              Expanded(
                child: TextFormField(
                  style: TextStyle(fontSize: 14),
                  controller: _ipController,
                  validator: _validateIp,
                  decoration: const InputDecoration(
                    hintText: '127.0.0.1',
                    isDense: true,
                    contentPadding: EdgeInsets.symmetric(horizontal: 12, vertical: 12),
                    border: OutlineInputBorder(),
                  ),
                ),
              ),
            ],
          ),
          const SizedBox(height: 4),

          Row(
            children: [
              const SizedBox(width: 0, height: 40),
              const SizedBox(
                width: 100,
                child: Text('Mode:', style: TextStyle(fontWeight: FontWeight.w500)),
              ),
              Radio<bool>(
                materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
                value: true,
                groupValue: !_isServer,
                onChanged: (val) => setState(() => _isServer = !val!),
              ),
              GestureDetector(
                onTap: () => setState(() => _isServer = false),
                child: const Text('Client'),
              ),
              const SizedBox(width: 16),
              Radio<bool>(
                materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
                value: false,
                groupValue: !_isServer,
                onChanged: (val) => setState(() => _isServer = !val!),
              ),
              GestureDetector(
                onTap: () => setState(() => _isServer = true),
                child: const Text('Server'),
              ),
            ],
          ),

          if (!_isServer) ...[
            const SizedBox(height: 4),
            Row(
              children: [
                const SizedBox(width: 0, height: 40),
                const SizedBox(
                  width: 100,
                  child: Text('Compression:', style: TextStyle(fontWeight: FontWeight.w500)),
                ),
                Switch(
                  value: _isCompress,
                  onChanged: _isTest ? null : (bool value) {
                    setState(() => _isCompress = value);
                  }
                )
              ],
            ),
            const SizedBox(height: 4),
            Row(
              children: [
                const SizedBox(
                  width: 100,
                  child: Text('Test Mode:', style: TextStyle(fontWeight: FontWeight.w500)),
                ),
                Switch(
                  value: _isTest,
                  onChanged: _isCompress ? null : (bool value) {
                    setState(() => _isTest = value);
                  }
                ),
              ],
            ),
          ],

          const Spacer(),

          ElevatedButton(
            onPressed: _onLaunch,
            style: ElevatedButton.styleFrom(
              padding: const EdgeInsets.symmetric(vertical: 14),
            ),
            child: const Text('Launch Engine', style: TextStyle(fontSize: 15)),
          ),
        ],
      ),
    );
  }
}