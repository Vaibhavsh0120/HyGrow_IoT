import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter_secure_storage/flutter_secure_storage.dart';

class ApiFailure implements Exception {
  const ApiFailure(
    this.message, {
    this.code = 'request_failed',
    this.requestId,
  });
  final String message;
  final String code;
  final String? requestId;
  @override
  String toString() =>
      requestId == null ? message : '$message\nReference: $requestId';
}

abstract interface class HyGrowApi {
  Future<Map<String, dynamic>> request(
    String path, {
    String method = 'GET',
    Map<String, dynamic>? body,
    String? token,
  });
}

class HttpHyGrowApi implements HyGrowApi {
  HttpHyGrowApi(this.baseUrl);
  final Uri baseUrl;
  final HttpClient _client = HttpClient()
    ..connectionTimeout = const Duration(seconds: 10);

  @override
  Future<Map<String, dynamic>> request(
    String path, {
    String method = 'GET',
    Map<String, dynamic>? body,
    String? token,
  }) async {
    final local = ['localhost', '127.0.0.1', '10.0.2.2'].contains(baseUrl.host);
    if (baseUrl.scheme != 'https' &&
        !(local &&
            (kDebugMode ||
                const bool.fromEnvironment('ALLOW_INSECURE_LOCAL')))) {
      throw const ApiFailure(
        'This connection requires HTTPS.',
        code: 'insecure_api',
      );
    }
    try {
      final request = await _client
          .openUrl(method, baseUrl.resolve('/api/v1$path'))
          .timeout(const Duration(seconds: 10));
      request.headers.contentType = ContentType.json;
      if (token != null) {
        request.headers.set(HttpHeaders.authorizationHeader, 'Bearer $token');
      }
      if (body != null) request.write(jsonEncode(body));
      final response = await request.close().timeout(
        const Duration(seconds: 15),
      );
      final bytes = <int>[];
      await for (final chunk in response.timeout(const Duration(seconds: 15))) {
        if (bytes.length + chunk.length > 1024 * 1024) {
          throw const ApiFailure('The response was too large. Please retry.');
        }
        bytes.addAll(chunk);
      }
      final decoded = jsonDecode(utf8.decode(bytes));
      if (decoded is! Map<String, dynamic>) throw const FormatException();
      if (response.statusCode >= 400) {
        final error = decoded['error'];
        throw ApiFailure(
          error is Map
              ? (error['message'] as String? ?? 'Please retry.')
              : 'Please retry.',
          code: error is Map
              ? (error['code'] as String? ?? 'request_failed')
              : 'request_failed',
          requestId: decoded['requestId'] as String?,
        );
      }
      return decoded;
    } on ApiFailure {
      rethrow;
    } on SocketException {
      throw const ApiFailure(
        'Unable to connect. Check your connection and try again.',
        code: 'offline',
      );
    } on TimeoutException {
      throw const ApiFailure(
        'The connection is taking too long. Please retry.',
        code: 'timeout',
      );
    } on HandshakeException {
      throw const ApiFailure(
        'A secure connection could not be established.',
        code: 'tls_error',
      );
    } on FormatException {
      throw const ApiFailure(
        'The service returned an unexpected response. Please retry.',
        code: 'invalid_response',
      );
    }
  }
}

abstract interface class SessionStore {
  Future<String?> read(String key);
  Future<void> write(String key, String value);
  Future<void> delete(String key);
}

class SecureSessionStore implements SessionStore {
  final FlutterSecureStorage _storage = const FlutterSecureStorage();
  @override
  Future<String?> read(String key) => _storage.read(key: key);
  @override
  Future<void> write(String key, String value) =>
      _storage.write(key: key, value: value);
  @override
  Future<void> delete(String key) => _storage.delete(key: key);
}

class AuthController extends ChangeNotifier {
  AuthController({required this.api, required this.store});
  final HyGrowApi api;
  final SessionStore store;
  static const _sessionKey = 'hygrow.session.v1';
  Map<String, dynamic>? _session;
  int _generation = 0;
  Future<void> _storageWrites = Future.value();
  bool restoring = true;
  bool offline = false;
  ThemeMode themeMode = ThemeMode.system;
  bool get signedIn => _session != null;
  Map<String, dynamic>? get user => _session?['user'] as Map<String, dynamic>?;
  String? get accessToken => _session?['accessToken'] as String?;

  Future<void> bootstrap() async {
    final generation = _generation;
    try {
      final theme = await store
          .read('hygrow.theme')
          .timeout(const Duration(seconds: 3));
      themeMode = ThemeMode.values.firstWhere(
        (value) => value.name == theme,
        orElse: () => ThemeMode.system,
      );
      final saved = await store
          .read(_sessionKey)
          .timeout(const Duration(seconds: 3));
      if (generation != _generation) return;
      if (saved != null) {
        final decoded = jsonDecode(saved);
        if (decoded is Map<String, dynamic> &&
            decoded['accessToken'] is String &&
            decoded['refreshToken'] is String &&
            decoded['user'] is Map<String, dynamic>) {
          _session = decoded;
        }
      }
    } catch (_) {
      if (generation != _generation) return;
      _session = null;
    }
    restoring = false;
    notifyListeners();
    if (_session != null) {
      final restoringSession = _session!;
      try {
        final me = await api.request('/me', token: accessToken);
        if (generation != _generation) return;
        await _accept({...restoringSession, 'user': me['user']}, generation);
      } on ApiFailure catch (error) {
        if (generation != _generation) return;
        if (error.code == 'unauthorized' || error.code == 'session_expired') {
          try {
            await _accept(
              await api.request(
                '/auth/refresh',
                method: 'POST',
                body: {'refreshToken': restoringSession['refreshToken']},
              ),
              generation,
            );
          } on ApiFailure catch (refreshError) {
            if (generation != _generation) return;
            if ([
              'unauthorized',
              'session_expired',
              'invalid_refresh',
              'refresh_reused',
            ].contains(refreshError.code)) {
              await signOut();
            } else {
              offline = true;
            }
          }
        } else {
          offline = true;
        }
      }
      notifyListeners();
    }
  }

  Future<void> _accept(Map<String, dynamic> session, int generation) async {
    if (generation != _generation) return;
    if (session['accessToken'] is! String ||
        session['refreshToken'] is! String ||
        session['user'] is! Map<String, dynamic>) {
      throw const ApiFailure('The session could not be verified.');
    }
    await _serializeStorage(() async {
      if (generation != _generation) return;
      await store.write(_sessionKey, jsonEncode(session));
      if (generation != _generation) return;
      _session = session;
      offline = false;
      notifyListeners();
    });
  }

  // Serialize persistence so a pending write cannot outlive logout's delete.
  Future<void> _serializeStorage(Future<void> Function() operation) {
    final next = _storageWrites.then(
      (_) => operation(),
      onError: (Object _) => operation(),
    );
    _storageWrites = next;
    return next;
  }

  Future<void> login(String email, String password) async {
    final generation = ++_generation;
    await _accept(
      await api.request(
        '/auth/login',
        method: 'POST',
        body: {'email': email.trim(), 'password': password},
      ),
      generation,
    );
  }

  Future<String> signup(String name, String email, String password) async {
    final result = await api.request(
      '/auth/signup',
      method: 'POST',
      body: {
        'displayName': name.trim(),
        'email': email.trim(),
        'password': password,
        'role': 'farmer',
      },
    );
    return result['challengeId'] as String;
  }

  Future<void> verify(String challengeId, String code) async {
    final generation = ++_generation;
    await _accept(
      await api.request(
        '/auth/verify',
        method: 'POST',
        body: {'challengeId': challengeId, 'code': code},
      ),
      generation,
    );
  }

  Future<String> forgot(String email) async =>
      (await api.request(
            '/auth/password/request',
            method: 'POST',
            body: {'email': email.trim()},
          ))['challengeId']
          as String;
  Future<void> reset(String challengeId, String code, String password) async {
    await api.request(
      '/auth/password/reset',
      method: 'POST',
      body: {'challengeId': challengeId, 'code': code, 'password': password},
    );
  }

  Future<List<dynamic>> devices() async =>
      (await api.request('/devices', token: accessToken))['devices']
          as List<dynamic>;
  Future<void> setTheme(ThemeMode value) async {
    themeMode = value;
    notifyListeners();
    await store.write('hygrow.theme', value.name);
  }

  Future<void> signOut() async {
    ++_generation;
    final token = accessToken;
    _session = null;
    offline = false;
    notifyListeners();
    await _serializeStorage(() => store.delete(_sessionKey));
    if (token != null) {
      unawaited(
        api
            .request('/auth/logout', method: 'POST', token: token)
            .then<void>((_) {})
            .catchError((Object _) {}),
      );
    }
  }
}
