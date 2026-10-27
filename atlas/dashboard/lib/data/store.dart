import 'dart:async';

import 'package:flutter/foundation.dart';

import 'models.dart';

enum Link { connecting, ok, lost }

/// 화면이 보는 상태 한 곳. D-Bus(기기)와 시연용 가상 데이터가 같은 모양으로 채운다.
abstract class NuniStore extends ChangeNotifier {
  Link link = Link.connecting;
  String? linkError;
  FusionState state = FusionState.empty;
  Sensors sensors = const Sensors();
  CryLogView? cry;
  List<AlertEvent> alerts = const [];

  /// 최근 15분 호흡수(레이더가 아기를 잡고 호흡을 잰 때만)
  final List<(DateTime, double)> breathHistory = [];
  String? _lastBreathSample;

  /// 답을 기록한 뒤 잠깐 보여 줄 확인 문구
  String? notice;
  bool noticeIsError = false;
  Timer? _noticeTimer;

  bool get isDemo => false;

  void start();

  /// 부모 답. 실패하면 사용자에게 보여 줄 문구를 돌려준다.
  Future<String?> answer(String id, String action);
  Future<String?> answerNot(String id, String action);
  Future<String?> logFeed();

  String label(String action) =>
      cry?.labels[action] ?? _labelsKo[action] ?? action;

  Radar? get radar => sensors.radar;

  /// 최근 3.5초 이내에 전달된 유효 레이더 관측인지
  bool get radarFresh {
    final r = sensors.radar;
    if (r == null) return false;
    final at = fromEpoch(r.ts);
    return at != null &&
        DateTime.now().difference(at).inMilliseconds >= 0 &&
        DateTime.now().difference(at).inMilliseconds <= 3500;
  }

  bool get cryFresh {
    final at = fromEpoch(sensors.cry?.ts);
    return at != null &&
        DateTime.now().difference(at).inMilliseconds >= 0 &&
        DateTime.now().difference(at).inMilliseconds <= 3500;
  }

  bool get breathObserved {
    final r = radar;
    final br = r?.breathingRate;
    return link == Link.ok &&
        radarFresh &&
        r?.presence == true &&
        r?.breathValid == true &&
        br != null &&
        br.isFinite &&
        br > 0;
  }

  bool get soundAvailable => link == Link.ok && cryFresh;

  bool get roomAvailable {
    final r = sensors.room;
    final at = fromEpoch(r?.ts);
    if (link != Link.ok || r == null || !r.valid || at == null) return false;
    final age = DateTime.now().difference(at).inMilliseconds;
    return age >= 0 && age <= 20000;
  }

  void recordBreath(Radar r) {
    final br = r.breathingRate;
    if (!r.presence || !r.breathValid || br == null || !br.isFinite || br <= 0)
      return;
    if (r.sampleId != null && r.sampleId == _lastBreathSample) return;
    final now = DateTime.now();
    if (breathHistory.isNotEmpty &&
        now.difference(breathHistory.last.$1).inMilliseconds < 900) return;
    breathHistory.add((now, br));
    _lastBreathSample = r.sampleId;
    breathHistory.removeWhere((e) => now.difference(e.$1).inMinutes >= 15);
  }

  void showNotice(String text, {bool error = false}) {
    notice = text;
    noticeIsError = error;
    _noticeTimer?.cancel();
    _noticeTimer = Timer(const Duration(seconds: 4), () {
      notice = null;
      notifyListeners();
    });
    notifyListeners();
  }

  @override
  void dispose() {
    _noticeTimer?.cancel();
    super.dispose();
  }
}

const _labelsKo = {
  'feed': '수유',
  'diaper': '기저귀',
  'hold': '안아주기·자세 바꾸기',
  'self': '저절로 그침',
  'sick': '아파 보임',
  'other': '기타',
  'unknown': '모름',
};
