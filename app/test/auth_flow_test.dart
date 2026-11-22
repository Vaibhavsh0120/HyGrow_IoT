import 'package:flutter_test/flutter_test.dart';
import 'package:hygrow/main.dart';
import 'package:hygrow/auth/auth_controller.dart';

class MemoryStore implements SessionStore {
  final values = <String, String>{};
  @override
  Future<String?> read(String key) async => values[key];
  @override
  Future<void> write(String key, String value) async {
    values[key] = value;
  }

  @override
  Future<void> delete(String key) async {
    values.remove(key);
  }
}

class RecordingApi implements HyGrowApi {
  int calls = 0;
  @override
  Future<Map<String, dynamic>> request(
    String path, {
    String method = 'GET',
    Map<String, dynamic>? body,
    String? token,
  }) async {
    calls++;
    throw const ApiFailure(
      'Check your email and password.',
      code: 'invalid_credentials',
    );
  }
}

void main() {
  testWidgets(
    'signed-out farmers land on role selection before authentication',
    (tester) async {
      final api = RecordingApi();
      await tester.pumpWidget(
        HyGrowApp(
          auth: AuthController(api: api, store: MemoryStore()),
        ),
      );
      await tester.pumpAndSettle();
      expect(find.text('Choose your role'), findsOneWidget);
      await tester.ensureVisible(find.text('Continue as Farmer'));
      await tester.tap(find.text('Continue as Farmer'));
      await tester.pumpAndSettle();
      expect(find.text('Welcome back'), findsOneWidget);
      await tester.tap(find.text('Log in'));
      await tester.pumpAndSettle();
      expect(api.calls, 0);
      expect(find.text('Enter your email address.'), findsOneWidget);
      expect(tester.takeException(), isNull);
    },
  );
}
