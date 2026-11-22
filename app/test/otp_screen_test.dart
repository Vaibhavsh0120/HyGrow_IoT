import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:hygrow/auth/auth_controller.dart';
import 'package:hygrow/screens/auth_screen.dart';
import 'package:hygrow/ui/theme.dart';

class EmptyStore implements SessionStore {
  @override
  Future<String?> read(String key) async => null;
  @override
  Future<void> write(String key, String value) async {}
  @override
  Future<void> delete(String key) async {}
}

class OtpApi implements HyGrowApi {
  final calls = <({String path, Map<String, dynamic>? body})>[];
  @override
  Future<Map<String, dynamic>> request(
    String path, {
    String method = 'GET',
    Map<String, dynamic>? body,
    String? token,
  }) async {
    calls.add((path: path, body: body));
    if (path == '/auth/otp/resend') {
      return {'challengeId': 'new-transaction', 'resendAfterSeconds': 60};
    }
    throw const ApiFailure(
      'This test stops before creating a session.',
      code: 'invalid_otp',
    );
  }
}

void main() {
  testWidgets(
    'OTP resend waits, requests once, and verifies the returned transaction',
    (tester) async {
      var now = DateTime.utc(2026, 11, 23);
      final api = OtpApi();
      await tester.pumpWidget(
        MaterialApp(
          theme: HyGrowTheme.light(),
          home: AuthScreen(
            auth: AuthController(api: api, store: EmptyStore()),
            step: AuthStep.verify,
            challengeId: 'old-transaction',
            email: 'farmer@example.test',
            now: () => now,
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(
        tester
            .widget<TextButton>(
              find.widgetWithText(TextButton, 'Resend in 60s'),
            )
            .onPressed,
        isNull,
      );
      expect(api.calls, isEmpty);
      now = now.add(const Duration(seconds: 61));
      await tester.pump(const Duration(seconds: 1));
      await tester.ensureVisible(find.text('Resend code'));
      await tester.tap(find.text('Resend code'));
      await tester.pumpAndSettle();
      expect(api.calls.single.body, {
        'challengeId': 'old-transaction',
        'purpose': 'signup',
      });
      expect(find.text('Resend in 60s'), findsOneWidget);
      await tester.enterText(find.byType(TextFormField), '123456');
      await tester.ensureVisible(find.text('Verify email'));
      await tester.tap(find.text('Verify email'));
      await tester.pumpAndSettle();
      expect(api.calls.last.body, {
        'challengeId': 'new-transaction',
        'code': '123456',
      });
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );
}
