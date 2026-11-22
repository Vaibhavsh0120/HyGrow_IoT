import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:hygrow/auth/auth_controller.dart';

void main() {
  test('empty logout POST does not advertise an absent JSON body', () async {
    final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    final observed = <String, dynamic>{};
    server.listen((request) async {
      observed['contentType'] = request.headers.contentType?.mimeType;
      observed['body'] = await utf8.decoder.bind(request).join();
      request.response.headers.contentType = ContentType.json;
      request.response.write('{"success":true}');
      await request.response.close();
    });
    try {
      final api = HttpHyGrowApi(Uri.parse('http://127.0.0.1:${server.port}'));
      await api.request('/auth/logout', method: 'POST', token: 'test-access');
      expect(observed['body'], isEmpty);
      expect(observed['contentType'], isNull);
    } finally {
      await server.close(force: true);
    }
  });
}
