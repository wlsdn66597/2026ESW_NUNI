import 'dart:convert';

import 'package:dbus/dbus.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:nuni_dashboard/data/dbus_store.dart';
import 'package:nuni_dashboard/data/store.dart';
import 'package:nuni_dashboard/ui/appliance_demo.dart';

class _FakeStore extends NuniStore {
  final calls = <int>[];
  @override
  void start() {}
  @override
  Future<String?> answer(String id, String action) async => null;
  @override
  Future<String?> answerNot(String id, String action) async => null;
  @override
  Future<String?> logFeed() async => null;
  @override
  Future<String?> setApplianceDemo(int seconds) async {
    calls.add(seconds);
    applianceDemoUntil =
        seconds > 0 ? DateTime.now().add(Duration(seconds: seconds)) : null;
    notifyListeners();
    return null;
  }
}

void main() {
  test('DbusStore starts and stops the appliance demo over D-Bus', () async {
    final sent = <(String, List<DBusValue>)>[];
    final store = DbusStore(call: (name, args) async {
      sent.add((name, args));
      return jsonEncode({'ok': true});
    });
    expect(await store.setApplianceDemo(30), isNull);
    expect(sent.single.$1, 'StartApplianceDemo');
    expect(sent.single.$2, [const DBusUint32(30)]);
    expect(store.applianceDemoActive, isTrue);
    expect(store.notice, contains('30초'));
    expect(await store.setApplianceDemo(0), isNull);
    expect(sent.last.$2, [const DBusUint32(0)]);
    expect(store.applianceDemoActive, isFalse);
    store.dispose();
  });

  test('service refusal or failure leaves the demo off and explains why', () async {
    var fail = false;
    final store = DbusStore(call: (name, args) async {
      if (fail) throw StateError('offline');
      return jsonEncode({'ok': false, 'error': 'MQTT 브로커에 연결되지 않았어요'});
    });
    expect(await store.setApplianceDemo(30), 'MQTT 브로커에 연결되지 않았어요');
    expect(store.applianceDemoActive, isFalse);
    expect(store.noticeIsError, isTrue);
    fail = true;
    expect(await store.setApplianceDemo(30), contains('가전 시연을 시작하지 못했어요'));
    expect(store.applianceDemoActive, isFalse);
    store.dispose();
  });

  testWidgets('button starts a 30 s demo, shows the countdown and can stop it',
      (tester) async {
    final store = _FakeStore()..link = Link.ok;
    await tester.pumpWidget(MaterialApp(
        home: Scaffold(
            body: ListenableBuilder(
                listenable: store,
                builder: (_, __) => ApplianceDemoButton(store: store)))));
    expect(find.text('가전 시연 30초'), findsOneWidget);
    await tester.tap(find.byKey(const ValueKey('appliance-demo')));
    await tester.pump();
    expect(store.calls, [30]);
    expect(find.textContaining('가전 시연 중'), findsOneWidget);
    expect(find.text('시연 끝내기'), findsOneWidget);
    await tester.tap(find.byKey(const ValueKey('appliance-demo')));
    await tester.pump();
    expect(store.calls, [30, 0]);
    expect(find.text('가전 시연 30초'), findsOneWidget);
    await tester.pump(const Duration(seconds: 2));
  });

  testWidgets('button is disabled while the service link is down', (tester) async {
    final store = _FakeStore()..link = Link.lost;
    await tester.pumpWidget(
        MaterialApp(home: Scaffold(body: ApplianceDemoButton(store: store))));
    await tester.tap(find.byKey(const ValueKey('appliance-demo')));
    await tester.pump();
    expect(store.calls, isEmpty);
  });
}
