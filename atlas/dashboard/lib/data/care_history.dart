import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'package:flutter/foundation.dart';
import 'models.dart';
import 'store.dart';
import 'history_models.dart';
export 'history_models.dart';

/// Example history is a separate display fixture, never fed to real learning or feedback.
HistoryView sampleHistory(DateTime now) {
  final day = DateTime(now.year, now.month, now.day);
  const hours = [
    18,
    19,
    20,
    18,
    19,
    20,
    18,
    19,
    20,
    18,
    19,
    20,
    6,
    7,
    2,
    10,
    14,
    16,
    21,
    18,
    19,
    20,
    8,
    11
  ];
  const actions = [
    'feed',
    'hold',
    'diaper',
    'feed',
    'feed',
    'hold',
    'feed',
    'diaper',
    'hold',
    'feed',
    'hold',
    'feed',
    'diaper',
    'feed',
    'hold',
    'feed',
    'diaper',
    'hold',
    'feed',
    'feed',
    'hold',
    'diaper',
    'feed',
    'hold'
  ];
  final records = [
    for (var i = 0; i < hours.length; i++)
      CareRecord(
          'sample-$i',
          day
              .subtract(Duration(days: 1 + i % 7))
              .add(Duration(hours: hours[i], minutes: 7 + i % 40)),
          90 + (i % 5) * 35,
          actions[i])
  ];
  records.sort((a, b) => b.at.compareTo(a.at));
  final env = [
    for (var i = 0; i < 18; i++)
      EnvironmentRecord(
          day
              .subtract(Duration(days: 1 + i % 7))
              .add(Duration(hours: 9 + i % 10)),
          22.3 + (i % 6) * .22,
          46 + (i % 5) * 1.6,
          610 + (i % 7) * 30,
          15 + i * 2)
  ];
  return HistoryView(records, env, sample: true);
}

class CareHistory extends ChangeNotifier {
  CareHistory(this.store, {this.filePath, DateTime Function()? clock})
      : now = clock ?? DateTime.now {
    store.addListener(_onStore);
    if (filePath == null) {
      _ready = true;
      _onStore();
    } else {
      unawaited(_load());
    }
  }
  final NuniStore store;
  final String? filePath;
  final DateTime Function() now;
  final _records = <String, CareRecord>{};
  final _environment = <EnvironmentRecord>[];
  final choices = <String, String>{};
  final _sending = <String>{}, _failed = <String>{};
  bool _ready = false, _disposed = false;
  DateTime? _quietSince, _lastSeen, _lastEnvironment;
  Future<void> _writes = Future.value();

  HistoryView get real {
    if (store.usesDatabaseHistory) {
      final data = store.recordedHistory ??
          const HistoryView([], [], database: true, available: false);
      return data.withStatus(
          loading: store.historyLoading && store.recordedHistory == null,
          error: store.historyError);
    }
    final cutoff = now().subtract(const Duration(days: 7));
    final records = _records.values.where((e) => e.at.isAfter(cutoff)).toList()
      ..sort((a, b) => b.at.compareTo(a.at));
    return HistoryView(
        records, _environment.where((e) => e.at.isAfter(cutoff)).toList());
  }

  Future<void> _load() async {
    try {
      final file = File(filePath!);
      if (await file.exists()) {
        final j = jsonDecode(await file.readAsString()) as Map<String, dynamic>;
        for (final row in (j['records'] as List? ?? [])) {
          final r = CareRecord.fromJson(Map<String, dynamic>.from(row as Map));
          _records.putIfAbsent(r.id, () => r);
        }
        for (final row in (j['environment'] as List? ?? [])) {
          _environment.add(EnvironmentRecord.fromJson(
              Map<String, dynamic>.from(row as Map)));
        }
        choices.addAll(Map<String, String>.from(j['choices'] as Map? ?? {}));
      }
    } catch (e) {
      debugPrint('Care history load: $e');
    }
    _ready = true;
    if (!_disposed) _onStore();
  }

  void _onStore() {
    if (_disposed || !_ready) return;
    var changed = false;
    for (final ep in store.cry?.episodes ?? <CryEpisode>[]) {
      final previous = _records[ep.id];
      if (!store.usesDatabaseHistory &&
          (previous == null ||
              (ep.action != null && previous.action != ep.action))) {
        _records[ep.id] =
            CareRecord(ep.id, ep.t0, ep.length.inSeconds, ep.action);
        changed = true;
      }
      final choice = choices[ep.id];
      if (choice != null &&
          !_sending.contains(ep.id) &&
          !_failed.contains(ep.id)) {
        if (ep.action == choice) {
          choices.remove(ep.id);
          changed = true;
        } else {
          unawaited(_submit(ep.id, choice));
        }
      }
    }
    final t = now();
    final quiet = store.soundAvailable && store.sensors.cry?.isCrying == false;
    if (!quiet ||
        (_lastSeen != null && t.difference(_lastSeen!).inSeconds > 5)) {
      _quietSince = null;
    }
    if (quiet) _quietSince ??= t;
    _lastSeen = t;
    final r = store.roomAvailable ? store.sensors.room : null;
    if (!store.usesDatabaseHistory &&
        r?.temp != null &&
        r?.humidity != null &&
        r?.co2 != null &&
        r!.temp!.isFinite &&
        r.humidity!.isFinite &&
        r.co2!.isFinite &&
        (_lastEnvironment == null ||
            t.difference(_lastEnvironment!).inSeconds >= 60)) {
      _environment.add(EnvironmentRecord(t, r.temp!, r.humidity!, r.co2!,
          _quietSince == null ? 0 : t.difference(_quietSince!).inMinutes));
      _lastEnvironment = t;
      changed = true;
    }
    if (changed) {
      final cutoff = t.subtract(const Duration(days: 7));
      _records.removeWhere((_, e) => e.at.isBefore(cutoff));
      _environment.removeWhere((e) => e.at.isBefore(cutoff));
      _save();
    }
    notifyListeners();
  }

  Future<String?> respond(String id, String action) async {
    choices[id] = action;
    _failed.remove(id);
    _save();
    if (store.cry?.current?.id == id) {
      store.showNotice('${store.label(action)} 선택 · 울음이 끝나면 저장해요');
      notifyListeners();
      return null;
    }
    return _submit(id, action);
  }

  Future<String?> _submit(String id, String action) async {
    if (_sending.contains(id)) return null;
    _sending.add(id);
    final error = await store.answer(id, action);
    _sending.remove(id);
    if (error == null) {
      choices.remove(id);
      _failed.remove(id);
      final old = _records[id];
      if (old != null) {
        _records[id] = CareRecord(old.id, old.at, old.seconds, action);
      }
    } else {
      _failed.add(id);
    }
    _save();
    if (!_disposed) notifyListeners();
    return error;
  }

  bool failed(String id) => _failed.contains(id);
  bool sending(String id) => _sending.contains(id);
  void _save() {
    if (filePath == null || !_ready) return;
    final payload = jsonEncode({
      'v': 1,
      'records': _records.values.map((e) => e.toJson()).toList(),
      'environment': _environment.map((e) => e.toJson()).toList(),
      'choices': choices
    });
    _writes = _writes.then((_) async {
      final file = File(filePath!);
      await file.parent.create(recursive: true);
      await file.writeAsString(payload, flush: true);
    }).catchError((Object e) {
      debugPrint('Care history save: $e');
    });
  }

  @override
  void dispose() {
    _disposed = true;
    store.removeListener(_onStore);
    super.dispose();
  }
}
