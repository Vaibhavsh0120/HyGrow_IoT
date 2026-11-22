import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:hygrow/ui/glass_nav_bar.dart';

void main() {
  testWidgets(
    'glass navigation reports selection and invokes the selected destination',
    (tester) async {
      var selected = -1;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            bottomNavigationBar: HyGrowNavBar(
              destinations: const [
                GlassDestination(icon: Icons.memory_rounded, label: 'Devices'),
                GlassDestination(
                  icon: Icons.spa_outlined,
                  label: 'Plant Tracker',
                ),
                GlassDestination(
                  icon: Icons.auto_awesome_outlined,
                  label: 'HyGrow AI',
                ),
                GlassDestination(
                  icon: Icons.storefront_outlined,
                  label: 'Market',
                ),
              ],
              selectedIndex: 0,
              onSelected: (index) => selected = index,
            ),
          ),
        ),
      );
      await tester.tap(find.text('Plant Tracker'));
      await tester.pumpAndSettle();
      expect(selected, 1);
      expect(find.text('Devices'), findsOneWidget);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets('four long labels fit on a small phone with large text', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(360, 800);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await tester.pumpWidget(
      MaterialApp(
        home: MediaQuery(
          data: const MediaQueryData(textScaler: TextScaler.linear(1.6)),
          child: Scaffold(
            bottomNavigationBar: HyGrowNavBar(
              destinations: const [
                GlassDestination(
                  icon: Icons.dashboard_outlined,
                  label: 'Dashboard',
                ),
                GlassDestination(
                  icon: Icons.tune_rounded,
                  label: 'Calibration',
                ),
                GlassDestination(
                  icon: Icons.terminal_rounded,
                  label: 'Terminal',
                ),
                GlassDestination(
                  icon: Icons.settings_outlined,
                  label: 'Settings',
                ),
              ],
              selectedIndex: 0,
              onSelected: (_) {},
            ),
          ),
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
  });
}
