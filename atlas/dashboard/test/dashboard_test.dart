import 'dart:io';
import 'dart:ui' as ui;
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:nuni_dashboard/main.dart';
import 'package:nuni_dashboard/data/demo_store.dart';
import 'package:nuni_dashboard/data/models.dart';
import 'package:nuni_dashboard/data/store.dart';

class _Store extends NuniStore {
  final answers = <(String, String)>[];
  @override
  void start() {}
  @override
  Future<String?> answer(String id, String action) async {
    answers.add((id, action));
    return null;
  }

  @override
  Future<String?> answerNot(String id, String action) async => null;
  @override
  Future<String?> logFeed() async => null;
}

Future<void> capture(WidgetTester tester, GlobalKey key, String name) async {
  await tester.runAsync(() async {
    final boundary =
        key.currentContext!.findRenderObject()! as RenderRepaintBoundary;
    final image = await boundary.toImage(pixelRatio: 1);
    final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
    Directory('test-output').createSync(recursive: true);
    File('test-output/$name.png').writeAsBytesSync(bytes!.buffer.asUint8List());
    image.dispose();
  });
}

Future<void> loadPenguin(WidgetTester tester) async {
  await tester.runAsync(() async {
    for (final name in [
      'nuni_penguin_black',
      'penguin_calm',
      'penguin_crying'
    ]) {
      await precacheImage(AssetImage('assets/images/$name.png'),
          tester.element(find.byType(Scaffold).first));
    }
  });
  await tester.pump(const Duration(milliseconds: 400));
}

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

  for (final scene in ['calm', 'crying', 'learning', 'question', 'alert']) {
    testWidgets('$scene fits and shows no movement or position claims',
        (tester) async {
      tester.view.physicalSize = const Size(1920, 1080);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final store = DemoStore(scene)..start();
      final key = GlobalKey();
      await tester
          .pumpWidget(RepaintBoundary(key: key, child: NuniApp(store: store)));
      await loadPenguin(tester);
      expect(tester.takeException(), isNull);
      expect(find.text('움직임'), findsNothing);
      expect(find.text('침대에 있어요'), findsNothing);
      expect(find.text('밝기'), findsNothing);
      expect(find.text('NUNI'), findsOneWidget);
      expect(find.byType(Image), findsOneWidget);
      expect(find.text('현재 상태'), findsOneWidget);
      expect(find.text('과거 기록'), findsOneWidget);
      expect(find.text('지난 7일 · 합성 예시'), findsOneWidget);
      expect(find.text('레이더 전파 정보'), findsOneWidget);
      expect(find.text('60GHz · 송신 전력 약 16mW (12dBm)'), findsOneWidget);
      expect(find.text('닿지 않고 살펴요'), findsNothing);
      expect(find.text('아기 곁의 소리를 듣고 있어요.'), findsNothing);
      await capture(tester, key, scene);
      if (scene == 'crying') {
        expect(find.text('어떤 돌봄을 하고 있나요?'), findsOneWidget);
        expect(find.byKey(const ValueKey('penguin-penguin_crying')),
            findsOneWidget);
        await tester.tap(find.byKey(const ValueKey('response-feed')));
        await tester.pump(const Duration(milliseconds: 400));
        expect(find.text('수유 선택 · 울음 종료 후 저장'), findsOneWidget);
        expect(tester.takeException(), isNull);
        await capture(tester, key, 'crying-response-selected');
      }
      await tester.pumpWidget(const SizedBox.shrink());
      store.dispose();
    });
  }

  testWidgets(
      'stale sound and invalid respiration never appear as current observations',
      (tester) async {
    tester.view.physicalSize = const Size(1920, 1080);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final now = DateTime.now().millisecondsSinceEpoch / 1000;
    final store = _Store()
      ..link = Link.ok
      ..cry = const CryLogView()
      ..sensors = Sensors(
          radar: Radar(
              breathingRate: 31, presence: true, breathValid: false, ts: now),
          cry: CrySound(isCrying: false, ts: now - 30),
          room: Room(temp: 22, ts: now - 30));
    final key = GlobalKey();
    await tester
        .pumpWidget(RepaintBoundary(key: key, child: NuniApp(store: store)));
    await loadPenguin(tester);
    expect(find.text('31'), findsNothing);
    expect(find.text('울음 없음'), findsNothing);
    expect(find.text('환경 센서의 새 정보를 기다려요'), findsOneWidget);
    expect(tester.takeException(), isNull);
    await capture(tester, key, 'stale');
    await tester.pumpWidget(const SizedBox.shrink());
    store.dispose();
  });

  testWidgets('missing radar keeps live sound and room visible',
      (tester) async {
    tester.view.physicalSize = const Size(1920, 1080);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final now = DateTime.now().millisecondsSinceEpoch / 1000;
    final store = _Store()
      ..link = Link.ok
      ..cry = const CryLogView()
      ..sensors = Sensors(
          cry: CrySound(ts: now),
          room: Room(temp: 21, humidity: 62, co2: 1800, ts: now))
      ..state = const FusionState(actions: ['ventilate']);
    final key = GlobalKey();
    await tester
        .pumpWidget(RepaintBoundary(key: key, child: NuniApp(store: store)));
    await loadPenguin(tester);
    expect(find.text('지금은 울음이 없어요'), findsOneWidget);
    expect(find.text('울음 없음'), findsOneWidget);
    expect(tester.takeException(), isNull);
    await capture(tester, key, 'no-radar');
    await tester.tap(find.text('실제'));
    await tester.pump(const Duration(milliseconds: 400));
    expect(find.text('최근 7일 · 실제 기록'), findsOneWidget);
    expect(find.text('지난 7일 · 합성 예시'), findsNothing);
    expect(find.text('환경 기록을 더 모으고 있어요'), findsOneWidget);
    expect(tester.takeException(), isNull);
    await capture(tester, key, 'real-history-empty');
    await tester.pumpWidget(const SizedBox.shrink());
    store.dispose();
  });

  testWidgets('radio evidence opens and closes at a small display size',
      (tester) async {
    tester.view.physicalSize = const Size(1024, 600);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final store = DemoStore('calm')..start();
    final key = GlobalKey();
    await tester
        .pumpWidget(RepaintBoundary(key: key, child: NuniApp(store: store)));
    await loadPenguin(tester);
    await tester.tap(find.text('과거 기록'));
    await tester.pump(const Duration(milliseconds: 400));
    await capture(tester, key, 'history-small');
    await tester.ensureVisible(find.byKey(const ValueKey('radio-info-button')));
    await tester.pump(const Duration(milliseconds: 400));
    await capture(tester, key, 'history-footer-small');
    await tester.tap(find.byKey(const ValueKey('radio-info-button')));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 400));
    expect(find.text('전파 안내와 검증 상태'), findsOneWidget);
    expect(find.text('완제품 전파 노출 시험: 아직 실시하지 않았어요'), findsOneWidget);
    expect(tester.takeException(), isNull);
    await capture(tester, key, 'radio-info-small');
    await tester.tap(find.byTooltip('닫기'));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 400));
    expect(find.text('전파 안내와 검증 상태'), findsNothing);
    await tester.pumpWidget(const SizedBox.shrink());
    store.dispose();
  });

  for (final scene in ['calm', 'crying']) {
    testWidgets('$scene has readable controls on a small display',
        (tester) async {
      tester.view.physicalSize = const Size(1024, 600);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final store = DemoStore(scene)..start();
      final key = GlobalKey();
      await tester
          .pumpWidget(RepaintBoundary(key: key, child: NuniApp(store: store)));
      await loadPenguin(tester);
      expect(find.byType(FittedBox), findsNothing);
      if (scene == 'crying') {
        final button = find.byKey(const ValueKey('response-feed'));
        expect(tester.getSize(button).height, greaterThanOrEqualTo(44));
        await tester.tap(button);
        await tester.pump(const Duration(milliseconds: 400));
        expect(find.text('수유 선택 · 울음 종료 후 저장'), findsOneWidget);
      }
      expect(tester.takeException(), isNull);
      await capture(tester, key, '$scene-small');
      await tester.pumpWidget(const SizedBox.shrink());
      store.dispose();
    });
  }

  testWidgets('lost service hides even recent cached observations',
      (tester) async {
    tester.view.physicalSize = const Size(1920, 1080);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final now = DateTime.now().millisecondsSinceEpoch / 1000;
    final store = _Store()
      ..link = Link.lost
      ..cry = const CryLogView()
      ..sensors = Sensors(
          radar: Radar(
              breathingRate: 31, presence: true, breathValid: true, ts: now),
          cry: CrySound(ts: now),
          room: Room(temp: 22, humidity: 50, co2: 600, ts: now));
    final key = GlobalKey();
    await tester
        .pumpWidget(RepaintBoundary(key: key, child: NuniApp(store: store)));
    await loadPenguin(tester);
    expect(find.text('연결을 확인해 주세요'), findsOneWidget);
    expect(find.text('31'), findsNothing);
    expect(find.text('울음 없음'), findsNothing);
    expect(tester.takeException(), isNull);
    await capture(tester, key, 'disconnected');
    await tester.pumpWidget(const SizedBox.shrink());
    store.dispose();
  });
}
