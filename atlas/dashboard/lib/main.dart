// 숨결지기 대시보드 — LG atlas(파이 5)의 Flutter 앱.
// 융합 서비스 com.atlas.NuniFusion1(D-Bus)에서 상태를 받아 보여 주고, 울음이 끝나면 "무엇으로 그쳤나요?" 답을 보낸다.
// 센서 없이 화면만 볼 때: flutter-atlas build atlas --ipk --release --dart-define=NUNI_DEMO=calm (data/demo_store.dart)
import 'package:flutter/material.dart';

import 'data/dbus_store.dart';
import 'data/demo_store.dart';
import 'data/store.dart';
import 'theme.dart';
import 'ui/dashboard.dart';

const _demo = String.fromEnvironment('NUNI_DEMO');

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  final NuniStore store = _demo.isEmpty ? DbusStore() : DemoStore(_demo);
  store.start();
  runApp(NuniApp(
      store: store,
      historyPath: store.isDemo
          ? null
          : '/data/share/usr/atlas/apps/com.atlas.app.nuni_dashboard/data/care-history.json'));
}

class NuniApp extends StatelessWidget {
  const NuniApp({super.key, required this.store, this.historyPath});
  final NuniStore store;
  final String? historyPath;

  @override
  Widget build(BuildContext context) => MaterialApp(
        title: 'NUNI',
        debugShowCheckedModeBanner: false,
        theme: nuniTheme(),
        home: Scaffold(
          backgroundColor: Palette.ink,
          body: Dashboard(store: store, historyPath: historyPath),
        ),
      );
}
