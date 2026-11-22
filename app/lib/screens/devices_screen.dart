import 'package:flutter/material.dart';
import 'package:go_router/go_router.dart';

import '../auth/auth_controller.dart';
import '../ui/glass.dart';
import '../ui/glass_nav_bar.dart';

class DevicesScreen extends StatefulWidget {
  const DevicesScreen({super.key, required this.auth});
  final AuthController auth;
  @override
  State<DevicesScreen> createState() => _DevicesScreenState();
}

class _DevicesScreenState extends State<DevicesScreen> {
  late Future<List<dynamic>> _devices;
  @override
  void initState() {
    super.initState();
    _devices = widget.auth.devices();
  }

  Future<void> _refresh() async {
    final request = widget.auth.devices();
    setState(() => _devices = request);
    await request;
  }

  @override
  Widget build(BuildContext context) => Scaffold(
    body: BotanicalCanvas(
      child: SafeArea(
        child: RefreshIndicator(
          onRefresh: _refresh,
          child: ListView(
            padding: const EdgeInsets.all(24),
            children: [
              Row(
                children: [
                  const BrandMark(size: 64, wordmark: false),
                  const SizedBox(width: 14),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text(
                          'HyGrow',
                          style: Theme.of(context).textTheme.headlineMedium,
                        ),
                        const Text('Your connected growing space'),
                      ],
                    ),
                  ),
                  GlassSurface(
                    padding: EdgeInsets.zero,
                    radius: 24,
                    child: IconButton(
                      tooltip: 'Account and settings',
                      onPressed: () => context.push('/settings'),
                      icon: const Icon(Icons.settings_outlined),
                    ),
                  ),
                ],
              ),
              const SizedBox(height: 36),
              Text(
                'Your devices',
                style: Theme.of(context).textTheme.headlineMedium,
              ),
              const SizedBox(height: 20),
              if (widget.auth.offline)
                const Padding(
                  padding: EdgeInsets.only(bottom: 16),
                  child: Text(
                    'Showing your saved account. We could not refresh it.',
                  ),
                ),
              FutureBuilder<List<dynamic>>(
                future: _devices,
                builder: (context, snapshot) {
                  if (snapshot.hasError) {
                    return GlassSurface(
                      child: Column(
                        children: [
                          const Icon(Icons.cloud_off_outlined, size: 36),
                          const SizedBox(height: 16),
                          const Text(
                            'We could not load your devices.',
                            textAlign: TextAlign.center,
                          ),
                          const SizedBox(height: 16),
                          FilledButton(
                            onPressed: () => _refresh(),
                            child: const Text('Try again'),
                          ),
                        ],
                      ),
                    );
                  }
                  if (!snapshot.hasData) {
                    return const GlassSurface(
                      child: SizedBox(
                        height: 130,
                        child: Center(child: CircularProgressIndicator()),
                      ),
                    );
                  }
                  final devices = snapshot.data!;
                  if (devices.isEmpty) {
                    return GlassSurface(
                      child: Column(
                        children: [
                          const Icon(Icons.spa_outlined, size: 48),
                          const SizedBox(height: 20),
                          Text(
                            'A fresh start',
                            style: Theme.of(context).textTheme.titleLarge,
                          ),
                          const SizedBox(height: 10),
                          const Text(
                            'No devices are linked to your account yet.',
                            textAlign: TextAlign.center,
                          ),
                          const SizedBox(height: 14),
                          const Text(
                            'Bluetooth and Wi-Fi setup arrive in the device-onboarding update.',
                            textAlign: TextAlign.center,
                          ),
                        ],
                      ),
                    );
                  }
                  return Column(
                    children: devices.map((item) {
                      final device = item as Map<String, dynamic>;
                      return Padding(
                        padding: const EdgeInsets.only(bottom: 16),
                        child: GlassSurface(
                          child: ListTile(
                            contentPadding: EdgeInsets.zero,
                            leading: const Icon(Icons.memory_rounded, size: 32),
                            title: Text(
                              device['name'] as String? ?? 'HyGrow device',
                            ),
                            subtitle: Text(
                              device['role'] == 'master'
                                  ? 'Master Owner'
                                  : 'Additional Owner',
                            ),
                          ),
                        ),
                      );
                    }).toList(),
                  );
                },
              ),
            ],
          ),
        ),
      ),
    ),
    bottomNavigationBar: HyGrowNavBar(
      destinations: const [
        GlassDestination(icon: Icons.memory_rounded, label: 'Devices'),
        GlassDestination(icon: Icons.spa_outlined, label: 'Plant Tracker'),
        GlassDestination(icon: Icons.auto_awesome_outlined, label: 'HyGrow AI'),
        GlassDestination(icon: Icons.storefront_outlined, label: 'Market'),
      ],
      selectedIndex: 0,
      onSelected: (index) {
        if (index != 0) {
          ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(
              content: Text('This feature is not available in this version.'),
            ),
          );
        }
      },
    ),
  );
}

class CommonSettingsScreen extends StatelessWidget {
  const CommonSettingsScreen({super.key, required this.auth});
  final AuthController auth;
  @override
  Widget build(BuildContext context) => Scaffold(
    body: BotanicalCanvas(
      child: SafeArea(
        child: ListView(
          padding: const EdgeInsets.all(24),
          children: [
            Row(
              children: [
                IconButton(
                  tooltip: 'Back',
                  onPressed: () => context.pop(),
                  icon: const Icon(Icons.arrow_back_ios_new_rounded),
                ),
                const SizedBox(width: 8),
                Text(
                  'Settings',
                  style: Theme.of(context).textTheme.headlineMedium,
                ),
              ],
            ),
            const SizedBox(height: 24),
            GlassSurface(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  Text(
                    'My profile',
                    style: Theme.of(context).textTheme.titleLarge,
                  ),
                  const SizedBox(height: 16),
                  Text(
                    auth.user?['displayName'] as String? ?? 'Farmer',
                    style: Theme.of(context).textTheme.titleMedium,
                  ),
                  const SizedBox(height: 8),
                  Text(auth.user?['email'] as String? ?? ''),
                ],
              ),
            ),
            const SizedBox(height: 20),
            GlassSurface(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  Text(
                    'Appearance',
                    style: Theme.of(context).textTheme.titleLarge,
                  ),
                  const SizedBox(height: 16),
                  RadioGroup<ThemeMode>(
                    groupValue: auth.themeMode,
                    onChanged: (value) {
                      if (value != null) auth.setTheme(value);
                    },
                    child: Column(
                      children: [
                        for (final value in ThemeMode.values)
                          RadioListTile<ThemeMode>(
                            value: value,
                            title: Text(switch (value) {
                              ThemeMode.system => 'Follow system',
                              ThemeMode.light => 'Light',
                              ThemeMode.dark => 'Neutral charcoal dark',
                            }),
                          ),
                      ],
                    ),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 24),
            OutlinedButton.icon(
              onPressed: () => auth.signOut(),
              icon: const Icon(Icons.logout_rounded),
              label: const Text('Log out'),
            ),
          ],
        ),
      ),
    ),
  );
}
