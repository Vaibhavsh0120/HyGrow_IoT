import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:hygrow/auth/auth_controller.dart';
import 'package:hygrow/screens/auth_screen.dart';
import 'package:hygrow/ui/theme.dart';

class NoApi implements HyGrowApi {
  @override
  Future<Map<String, dynamic>> request(
    String path, {
    String method = 'GET',
    Map<String, dynamic>? body,
    String? token,
  }) async => throw const ApiFailure('Test does not send');
}

class NoStore implements SessionStore {
  @override
  Future<String?> read(String key) async => null;
  @override
  Future<void> write(String key, String value) async {}
  @override
  Future<void> delete(String key) async {}
}

void main() {
  for (final step in AuthStep.values) {
    testWidgets('$step remains usable with double text on a small phone', (
      tester,
    ) async {
      tester.view.physicalSize = const Size(360, 800);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final semantics = tester.ensureSemantics();
      await tester.pumpWidget(
        MaterialApp(
          theme: HyGrowTheme.dark(),
          builder: (context, child) => MediaQuery(
            data: MediaQuery.of(context)
                .copyWith(textScaler: const TextScaler.linear(2)),
            child: child!,
          ),
          home: AuthScreen(
            auth: AuthController(api: NoApi(), store: NoStore()),
            step: step,
          ),
        ),
      );
      await tester.pumpAndSettle();
      if ([AuthStep.login, AuthStep.signup, AuthStep.forgot].contains(step)) {
        expect(find.bySemanticsLabel('Email address'), findsOneWidget);
      }
      final exception = tester.takeException();
      semantics.dispose();
      expect(exception, isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    });
  }
}
