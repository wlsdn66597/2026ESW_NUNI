import 'package:flutter_test/flutter_test.dart';
import 'package:nuni_dashboard/data/care_history.dart';
import 'package:nuni_dashboard/data/models.dart';
import 'package:nuni_dashboard/data/store.dart';

class FakeStore extends NuniStore {
  final submitted = <(String, String)>[];
  String? failure;
  @override
  void start() {}
  @override
  Future<String?> answer(String id, String action) async {
    submitted.add((id, action));
    return failure;
  }

  @override
  Future<String?> answerNot(String id, String action) async => null;
  @override
  Future<String?> logFeed() async => null;
  void refresh() => notifyListeners();
}

void main() {
  test(
      'parent response during crying is submitted only after matching episode ends',
      () async {
    final start = DateTime.now().subtract(const Duration(minutes: 1));
    final store = FakeStore()
      ..link = Link.ok
      ..cry = CryLogView(current: CryNow(id: 'cry-1', t0: start));
    final history = CareHistory(store);
    await history.respond('cry-1', 'feed');
    expect(store.submitted, isEmpty);
    expect(history.real.records, isEmpty);
    store.cry = CryLogView(
        episodes: [CryEpisode(id: 'cry-1', t0: start, t1: DateTime.now())],
        pending: 'cry-1');
    store.refresh();
    await Future<void>.delayed(Duration.zero);
    expect(store.submitted, [('cry-1', 'feed')]);
    expect(history.real.records.single.action, 'feed');
    expect(history.choices, isEmpty);
    store.refresh();
    expect(store.submitted.length, 1);
    expect(history.real.records.single.action, 'feed');
    history.dispose();
    store.dispose();
  });

  test(
      'failed feedback remains visible and retries only on an explicit response',
      () async {
    final t = DateTime.now();
    final store = FakeStore()
      ..failure = 'temporary failure'
      ..cry = CryLogView(episodes: [CryEpisode(id: 'closed', t0: t, t1: t)]);
    final history = CareHistory(store);
    await history.respond('closed', 'hold');
    expect(history.failed('closed'), true);
    store.refresh();
    expect(store.submitted.length, 1);
    store.failure = null;
    await history.respond('closed', 'hold');
    expect(store.submitted.length, 2);
    expect(history.choices, isEmpty);
    history.dispose();
    store.dispose();
  });

  test(
      'synthetic hourly pattern and environment recommendation use separate fixture records',
      () {
    final fixture = sampleHistory(DateTime(2026, 10, 25, 22));
    expect(fixture.sample, true);
    expect(fixture.records.length, 24);
    expect(fixture.hourly.fold(0, (int a, int b) => a + b), 24);
    expect(fixture.peakBlock, 6);
    expect(fixture.temperatureRange, '22–24℃');
    expect(fixture.humidityRange, '45–55%');
    final store = FakeStore();
    final history = CareHistory(store);
    expect(history.real.records, isEmpty);
    expect(history.real.environment, isEmpty);
    expect(history.real.temperatureRange, isNull);
    history.dispose();
    store.dispose();
  });
}
