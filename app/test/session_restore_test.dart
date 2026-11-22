import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:hygrow/auth/auth_controller.dart';

final savedSession = <String, dynamic>{
  'accessToken': 'expired-access',
  'refreshToken': 'valid-refresh',
  'user': <String, dynamic>{'id': 'farmer-1', 'displayName': 'Farmer'},
};

class TestStore implements SessionStore {
  final values = {'hygrow.session.v1': jsonEncode(savedSession)};
  @override
  Future<String?> read(String key) async => values[key];
  @override
  Future<void> write(String key, String value) async => values[key] = value;
  @override
  Future<void> delete(String key) async => values.remove(key);
}

class RestoreApi implements HyGrowApi {
  RestoreApi({this.expiredAccess = false});
  final bool expiredAccess;
  final entered = Completer<void>();
  final response = Completer<Map<String, dynamic>>();
  @override
  Future<Map<String, dynamic>> request(
    String path, {
    String method = 'GET',
    Map<String, dynamic>? body,
    String? token,
  }) async {
    if (path == '/auth/logout') return {};
    if (path == '/me' && expiredAccess) {
      throw const ApiFailure('Expired', code: 'session_expired');
    }
    entered.complete();
    return response.future;
  }
}

void main() {
  for (final expiredAccess in [false, true]) {
    test('logout wins over pending restore expired=$expiredAccess', () async {
      final store = TestStore();
      final api = RestoreApi(expiredAccess: expiredAccess);
      final auth = AuthController(api: api, store: store);
      final restore = auth.bootstrap();
      await api.entered.future;
      await auth.signOut();
      api.response.complete(
        expiredAccess ? savedSession : {'user': savedSession['user']},
      );
      await restore;
      expect(auth.signedIn, isFalse);
      expect(store.values.containsKey('hygrow.session.v1'), isFalse);
    });
  }
  test(
    'temporary refresh outage preserves saved session and marks offline',
    () async {
      final store = TestStore();
      final api = RestoreApi(expiredAccess: true);
      final auth = AuthController(api: api, store: store);
      final restore = auth.bootstrap();
      await api.entered.future;
      api.response.completeError(const ApiFailure('Offline', code: 'offline'));
      await restore;
      expect(auth.signedIn, isTrue);
      expect(auth.offline, isTrue);
      expect(store.values.containsKey('hygrow.session.v1'), isTrue);
    },
  );
}
