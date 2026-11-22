import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:hygrow/screens/welcome_screen.dart';
import 'package:hygrow/ui/theme.dart';

void main() {
  testWidgets('Farmer selection invokes authentication entry once', (
    tester,
  ) async {
    var selections = 0;
    await tester.pumpWidget(
      MaterialApp(
        theme: HyGrowTheme.light(),
        home: WelcomeScreen(onFarmer: () => selections++),
      ),
    );
    await tester.pumpAndSettle();
    await tester.ensureVisible(find.text('Continue as Farmer'));
    await tester.tap(find.text('Continue as Farmer'));
    expect(selections, 1);
    expect(find.text('Choose your role'), findsOneWidget);
    expect(tester.takeException(), isNull);
  });
  for (final size in [const Size(360, 800), const Size(1024, 768)]) {
    for (final dark in [false, true]) {
      testWidgets('welcome fits $size dark=$dark with large text', (
        tester,
      ) async {
        var selections = 0;
        tester.view.physicalSize = size;
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        await tester.pumpWidget(
          MaterialApp(
            theme: dark ? HyGrowTheme.dark() : HyGrowTheme.light(),
            builder: (context, child) => MediaQuery(
              data: MediaQuery.of(context)
                  .copyWith(textScaler: const TextScaler.linear(1.5)),
              child: child!,
            ),
            home: WelcomeScreen(onFarmer: () => selections++),
          ),
        );
        await tester.pumpAndSettle();
        expect(find.text('Choose your role'), findsOneWidget);
        await tester.ensureVisible(find.text('Continue as Farmer'));
        await tester.tap(find.text('Continue as Farmer'));
        expect(selections, 1);
        expect(tester.takeException(), isNull);
      });
    }
  }
}
