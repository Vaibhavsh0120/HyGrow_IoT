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

class ConcurrentRefreshApi implements HyGrowApi {
  int refreshCalls = 0;
  final refresh = Completer<Map<String, dynamic>>();
  @override
  Future<Map<String, dynamic>> request(
    String path, {
    String method = 'GET',
    Map<String, dynamic>? body,
    String? token,
  }) async {
    if (path == '/auth/login') return savedSession;
    if (path == '/auth/refresh') {
      refreshCalls++;
      return refresh.future;
    }
    if (path == '/devices' && token == 'fresh-access') {
      return {'devices': <dynamic>[]};
    }
    throw const ApiFailure('Expired', code: 'unauthorized');
  }
}

class FailingDeleteStore extends TestStore {
  @override
  Future<void> delete(String key) async =>
      throw StateError('secure-store-unavailable');
}

class LogoutApi implements HyGrowApi {
  int logoutCalls = 0;
  @override
  Future<Map<String, dynamic>> request(
    String path, {
    String method = 'GET',
    Map<String, dynamic>? body,
    String? token,
  }) async {
    if (path == '/auth/logout') {
      logoutCalls++;
      return {};
    }
    return savedSession;
  }
}

class FailingCleanupStore extends TestStore {
  bool fail = false;
  @override
  Future<void> delete(String key) async =>
      throw StateError('secure-store-unavailable');
  @override
  Future<void> write(String key, String value) async {
    if (fail) throw StateError('secure-store-unavailable');
    return super.write(key, value);
  }
}

void main() {
  test('failed persistent cleanup is explicit and retryable', () async {
    final store = FailingCleanupStore();
    final api = LogoutApi();
    final auth = AuthController(api: api, store: store);
    await auth.login('farmer@example.test', 'test-password');
    store.fail = true;
    await auth.signOut();
    expect(api.logoutCalls, 1);
    expect(auth.signOutWarning, isNotNull);
    store.fail = false;
    await auth.signOut();
    expect(auth.signOutWarning, isNull);
    expect(store.values['hygrow.session.v1'], 'null');
    final restored = AuthController(api: api, store: store);
    await restored.bootstrap();
    expect(restored.signedIn, isFalse);
  });
  test(
    'server logout is attempted even when local secure deletion fails',
    () async {
      final api = LogoutApi();
      final auth = AuthController(api: api, store: FailingDeleteStore());
      await auth.login('farmer@example.test', 'test-password');
      try {
        await auth.signOut();
      } catch (_) {
        /* Observe the previous failure path. */
      }
      await Future<void>.delayed(Duration.zero);
      expect(api.logoutCalls, 1);
      expect(auth.signedIn, isFalse);
    },
  );
  test('concurrent expired requests share one refresh and use new access credentials', () async {
    final api = ConcurrentRefreshApi();
    final auth = AuthController(api: api, store: TestStore());
    await auth.login('farmer@example.test', 'test-password');
    final results = Future.wait([auth.devices(), auth.devices()])
        .catchError((Object _) => <List<dynamic>>[]);
    await Future<void>.delayed(Duration.zero);
    expect(api.refreshCalls, 1);
    api.refresh.complete({
      ...savedSession,
      'accessToken': 'fresh-access',
      'refreshToken': 'fresh-refresh',
    });
    expect((await results).length, 2);
    expect(auth.accessToken, 'fresh-access');
  });
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
