import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:ui' as ui;
import 'package:dbus/dbus.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:nuni_dashboard/data/care_history.dart';
import 'package:nuni_dashboard/data/dbus_store.dart';
import 'package:nuni_dashboard/data/models.dart';
import 'package:nuni_dashboard/main.dart';

Map<String, dynamic> response() => {
      'ok': true,
      'time_unit': 'unix_seconds',
      'timezone': 'Asia/Seoul',
      'total_count': 61,
      'hourly': [
        for (var i = 0; i < 24; i++)
          i == 18
              ? 60
              : i == 19
                  ? 1
                  : 0
      ],
      'episodes': [
        {
          'id': 'db-only',
          't0': 1793460000.0,
          't1': 1793460090.0,
          'action': 'feed'
        }
      ],
      'quiet': {
        'count': 3,
        'temperature_c': {'min': 22.1, 'max': 24.4},
        'humidity_pct': {'min': 46.0, 'max': 53.0},
        'co2_ppm': {'min': 500, 'max': 700}
      }
    };

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUpAll(() async {
    for (final entry in {
      'NotoSansKR': 'assets/fonts/NotoSansKR-VF.ttf',
      'NotoSerifKR': 'assets/fonts/NotoSerifKR-VF.ttf',
      'MaterialIcons': 'fonts/MaterialIcons-Regular.otf'
    }.entries) {
      await (FontLoader(entry.key)..addFont(rootBundle.load(entry.value)))
          .load();
    }
  });
  test(
      'database model uses whole-range counts and observed environment extrema',
      () {
    final view = HistoryView.fromDatabase(response());
    expect(view.totalCount, 61);
    expect(view.records.length, 1);
    expect(view.hourly[18], 60);
    expect(view.peakBlock, 6);
    expect(view.records.single.at.millisecondsSinceEpoch, 1793460000000);
    expect(view.records.single.seconds, 90);
    expect(view.quietCount, 3);
    expect(view.temperatureRange, '22–25℃');
    expect(view.humidityRange, '45–55%');
    final limited = response()..['limited_by_size'] = true;
    expect(HistoryView.fromDatabase(limited).limitedBySize, true);
  });

  test(
      'invalid summary is rejected rather than displayed as genuine empty data',
      () {
    final bad = response()..['hourly'] = [1];
    expect(() => HistoryView.fromDatabase(bad), throwsFormatException);
    final inconsistent = response()..['total_count'] = 0;
    expect(() => HistoryView.fromDatabase(inconsistent), throwsFormatException);
    final invalidRange = response();
    (invalidRange['quiet'] as Map)['temperature_c'] = {'min': 30, 'max': 20};
    expect(() => HistoryView.fromDatabase(invalidRange), throwsFormatException);
  });

  test(
      'D-Bus polling requests seven days and keeps explicit stale data on error',
      () async {
    var failure = false;
    final calls = <(String, List<DBusValue>)>[];
    final at = DateTime.fromMillisecondsSinceEpoch(1793460200000);
    final store = DbusStore(
        clock: () => at,
        call: (name, args) async {
          calls.add((name, args));
          if (failure) throw StateError('offline');
          return jsonEncode(response());
        });
    final history = CareHistory(store);
    // GetCryLog's live cache must not populate the DB-backed actual tab.
    store.cry = CryLogView(episodes: [CryEpisode(id: 'local', t0: at, t1: at)]);
    expect(history.real.records, isEmpty);
    expect(history.real.loading, true);
    expect(history.real.available, false);
    await store.refreshHistory();
    expect(calls.single.$1, 'GetHistorySummary');
    expect(calls.single.$2[0].asInt64(), 1793460201 - 7 * 86400);
    expect(calls.single.$2[1].asInt64(), 1793460201);
    expect(calls.single.$2[2].asUint32(), 1000);
    expect(history.real.totalCount, 61);
    expect(history.real.records.single.id, 'db-only');
    failure = true;
    await store.refreshHistory();
    expect(history.real.totalCount, 61);
    expect(history.real.error, isNotNull);
    failure = false;
    await store.refreshHistory();
    expect(history.real.error, isNull);
    history.dispose();
    store.dispose();
  });

  test('overlapping polls and disposed responses do not update the UI',
      () async {
    final pending = Completer<String>();
    var calls = 0, changes = 0;
    final store = DbusStore(call: (_, __) {
      calls++;
      return pending.future;
    });
    store.addListener(() => changes++);
    final first = store.refreshHistory();
    await store.refreshHistory();
    expect(calls, 1);
    final before = changes;
    store.dispose();
    pending.complete(jsonEncode(response()));
    await first;
    expect(changes, before);
    expect(store.recordedHistory, isNull);
  });

  testWidgets('real device opens database history and renders complete counts',
      (tester) async {
    tester.view.physicalSize = const Size(1600, 1100);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final store = DbusStore(call: (_, __) async => jsonEncode(response()));
    await store.refreshHistory();
    final key = GlobalKey();
    await tester
        .pumpWidget(RepaintBoundary(key: key, child: NuniApp(store: store)));
    await tester.runAsync(() async {
      await precacheImage(
          const AssetImage('assets/images/nuni_penguin_black.png'),
          tester.element(find.byType(Scaffold).first));
    });
    await tester.pumpAndSettle();
    expect(find.text('최근 7일 · 실제 기록'), findsOneWidget);
    expect(find.text('지난 7일 · 합성 예시'), findsNothing);
    expect(find.text('61회'), findsOneWidget);
    expect(find.text('22–25℃  ·  습도 45–55%'), findsOneWidget);
    expect(tester.takeException(), isNull);
    const path = String.fromEnvironment('NUNI_HISTORY_PREVIEW');
    if (path.isNotEmpty) {
      await tester.runAsync(() async {
        final boundary =
            key.currentContext!.findRenderObject()! as RenderRepaintBoundary;
        final image = await boundary.toImage(pixelRatio: 1);
        final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
        await File(path).writeAsBytes(bytes!.buffer.asUint8List());
        image.dispose();
      });
    }
    await tester.pumpWidget(const SizedBox.shrink());
    store.dispose();
  });
}
