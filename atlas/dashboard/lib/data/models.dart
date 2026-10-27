// 서비스(com.atlas.NuniFusion1)가 주는 JSON을 화면에서 쓰기 쉬운 값으로 바꾼다.
// 키 이름은 nunifusion의 MQTT 메시지·D-Bus 응답과 같다(atlas/nuni/README.md).

double? _num(dynamic v) => v is num ? v.toDouble() : null;
List<String> _strs(dynamic v) =>
    v is List ? v.whereType<String>().toList() : const [];
Map<String, dynamic> _map(dynamic v) =>
    v is Map ? v.cast<String, dynamic>() : const {};

DateTime? fromEpoch(double? s) =>
    s == null ? null : DateTime.fromMillisecondsSinceEpoch((s * 1000).round());

/// sensor/radar — MR60BHA2 레이더(호흡·움직임·재실)
class Radar {
  const Radar({
    this.breathingRate,
    this.movement,
    this.presence = false,
    this.apnea = false,
    this.breathValid = false,
    this.distance,
    this.ts = 0,
    this.sampleId,
  });

  final double? breathingRate; // 회/분
  final double? movement; // 0~1
  final bool presence;
  final bool apnea;
  final bool breathValid; // false면 호흡을 못 잰 구간(0은 무호흡이 아니라 미측정)
  final double? distance; // cm
  final double ts;
  final String? sampleId;

  factory Radar.fromJson(Map<String, dynamic> j) => Radar(
        breathingRate: _num(j['breathing_rate'] ?? j['breathing_bpm']),
        movement: _num(j['movement']),
        presence: j['presence'] == true,
        apnea: j['apnea'] == true,
        breathValid: j['breath_valid'] == true,
        distance: _num(j['distance']),
        ts: _num(j['ts']) ?? 0,
        sampleId: j['sample_id']?.toString(),
      );
}

/// audio/cry — 마이크 울음 판정(1초마다)
class CrySound {
  const CrySound({this.isCrying = false, this.confidence = 0, this.ts = 0});
  final bool isCrying;
  final double confidence;
  final double ts;

  factory CrySound.fromJson(Map<String, dynamic> j) => CrySound(
        isCrying: j['is_crying'] == true,
        confidence: _num(j['confidence']) ?? 0,
        ts: _num(j['ts']) ?? 0,
      );
}

/// sensor/env — 방 환경(ESP32 + SCD41 등)
class Room {
  const Room(
      {this.temp,
      this.humidity,
      this.co2,
      this.lux,
      this.ts = 0,
      this.valid = true});
  final double? temp, humidity, co2, lux;
  final double ts;
  final bool valid;

  factory Room.fromJson(Map<String, dynamic> j) => Room(
        temp: _num(j['temp']),
        humidity: _num(j['humidity']),
        co2: _num(j['co2']),
        lux: _num(j['lux']),
        ts: _num(j['ts']) ?? 0,
        valid: j['valid'] == true,
      );
}

class Sensors {
  const Sensors({this.radar, this.cry, this.room});
  final Radar? radar;
  final CrySound? cry;
  final Room? room;

  factory Sensors.fromJson(Map<String, dynamic> j) => Sensors(
        radar: j['radar'] is Map ? Radar.fromJson(_map(j['radar'])) : null,
        cry: j['cry'] is Map ? CrySound.fromJson(_map(j['cry'])) : null,
        room: j['env'] is Map ? Room.fromJson(_map(j['env'])) : null,
      );
}

/// fusion/state — 융합 판단(위험도·수면 상태·환경 권고)
class FusionState {
  const FusionState({
    this.level = 'normal',
    this.reasons = const [],
    this.sleepState = 'unknown',
    this.sleepReason = '',
    this.actions = const [],
    this.ts = 0,
  });

  final String level; // normal · attention · alert
  final List<String> reasons;
  final String sleepState; // calm_sleep · restless · awake · unknown · hold
  final String sleepReason;
  final List<String>
      actions; // ventilate · humidify · dehumidify · cool · heat · dim_light
  final double ts;

  static const empty = FusionState();

  factory FusionState.fromJson(Map<String, dynamic> j) {
    final sleep = _map(j['sleep']);
    final acts = <String>[];
    if (j['actions'] is List) {
      for (final a in j['actions'] as List) {
        if (a is List && a.isNotEmpty && a.first is String)
          acts.add(a.first as String);
      }
    }
    return FusionState(
      level: j['level'] is String ? j['level'] as String : 'normal',
      reasons: _strs(j['reasons']),
      sleepState:
          sleep['state'] is String ? sleep['state'] as String : 'unknown',
      sleepReason: sleep['reason'] is String ? sleep['reason'] as String : '',
      actions: acts,
      ts: _num(j['ts']) ?? 0,
    );
  }
}

/// 울음 1·2순위 힌트(cry_personal.hint와 같은 키)
class CryHint {
  const CryHint({
    this.top2 = const [],
    this.probs = const {},
    this.show = false,
    this.show2 = false,
    this.nFeedback = 0,
    this.hoursSinceFeed,
    this.notTried = const [],
  });

  final List<String> top2;
  final Map<String, double> probs;
  final bool show, show2;
  final int nFeedback;
  final double? hoursSinceFeed;
  final List<String> notTried; // 이 울음에서 '해 봤는데 안 그쳤어요'라고 누른 행동

  factory CryHint.fromJson(Map<String, dynamic> j) => CryHint(
        top2: _strs(j['top2']),
        probs: _map(j['probs']).map((k, v) => MapEntry(k, _num(v) ?? 0)),
        show: j['show'] == true,
        show2: j['show2'] == true,
        nFeedback: (_num(j['n_feedback']) ?? 0).round(),
        hoursSinceFeed: _num(j['hours_since_feed']),
        notTried: _strs(j['not']),
      );
}

class CryEpisode {
  const CryEpisode({
    required this.id,
    required this.t0,
    required this.t1,
    this.n = 0,
    this.peak = 0,
    this.action,
    this.notTried = const [],
    this.hintTop2 = const [],
  });

  final String id;
  final DateTime t0, t1;
  final int n;
  final double peak;
  final String? action; // 답(없으면 null)
  final List<String> notTried;
  final List<String> hintTop2; // 울음 시작 때 낸 1·2순위

  Duration get length => t1.difference(t0);

  factory CryEpisode.fromJson(Map<String, dynamic> j) => CryEpisode(
        id: j['id'] as String? ?? '',
        t0: fromEpoch(_num(j['t0'])) ?? DateTime.now(),
        t1: fromEpoch(_num(j['t1'])) ?? DateTime.now(),
        n: (_num(j['n']) ?? 0).round(),
        peak: _num(j['peak']) ?? 0,
        action: j['action'] is String ? j['action'] as String : null,
        notTried: _strs(j['not']),
        hintTop2: _strs(_map(j['hint'])['top2']),
      );
}

class CryNow {
  const CryNow(
      {required this.id, required this.t0, this.seconds = 0, this.hint});
  final String id;
  final DateTime t0;
  final double seconds;
  final CryHint? hint;

  factory CryNow.fromJson(Map<String, dynamic> j) => CryNow(
        id: j['id'] as String? ?? '',
        t0: fromEpoch(_num(j['t0'])) ?? DateTime.now(),
        seconds: _num(j['seconds']) ?? 0,
        hint: j['hint'] is Map ? CryHint.fromJson(_map(j['hint'])) : null,
      );
}

/// GetCryLog — 울음 기록 상태
class CryLogView {
  const CryLogView({
    this.actions = const ['feed', 'diaper', 'hold'],
    this.answers = const [
      'feed',
      'diaper',
      'hold',
      'self',
      'sick',
      'other',
      'unknown'
    ],
    this.labels = const {},
    this.kMin = 10,
    this.nLabeled = 0,
    this.nAnswered = 0,
    this.lastFeed,
    this.current,
    this.pending,
    this.hitsN = 0,
    this.hitsTop1 = 0,
    this.hitsTop2 = 0,
    this.episodes = const [],
    this.recording = true,
  });

  final List<String> actions; // 1·2순위로 보여 주는 행동
  final List<String> answers; // 답 버튼 순서(행동 + 기록만 하는 답)
  final Map<String, String> labels;
  final int kMin; // 힌트를 보여 주기 시작하는 답 수
  final int nLabeled, nAnswered;
  final DateTime? lastFeed;
  final CryNow? current;
  final String? pending; // 답을 기다리는 울음 id
  final int hitsN, hitsTop1, hitsTop2;
  final List<CryEpisode> episodes; // 오래된 것 → 최근
  final bool recording;

  CryEpisode? episode(String? id) {
    if (id == null) return null;
    for (final e in episodes.reversed) {
      if (e.id == id) return e;
    }
    return null;
  }

  factory CryLogView.fromJson(Map<String, dynamic> j) {
    final hits = _map(j['hits']);
    return CryLogView(
      actions: _strs(j['actions']).isEmpty
          ? const ['feed', 'diaper', 'hold']
          : _strs(j['actions']),
      answers: _strs(j['answers']).isEmpty
          ? const ['feed', 'diaper', 'hold', 'self', 'sick', 'other', 'unknown']
          : _strs(j['answers']),
      labels: _map(j['labels']).map((k, v) => MapEntry(k, '$v')),
      kMin: (_num(j['k_min']) ?? 10).round(),
      nLabeled: (_num(j['n_labeled']) ?? 0).round(),
      nAnswered: (_num(j['n_answered']) ?? 0).round(),
      lastFeed: fromEpoch(_num(j['last_feed'])),
      current: j['current'] is Map ? CryNow.fromJson(_map(j['current'])) : null,
      pending: j['pending'] is String ? j['pending'] as String : null,
      hitsN: (_num(hits['n']) ?? 0).round(),
      hitsTop1: (_num(hits['top1']) ?? 0).round(),
      hitsTop2: (_num(hits['top2']) ?? 0).round(),
      episodes: j['episodes'] is List
          ? (j['episodes'] as List)
              .whereType<Map>()
              .map((e) => CryEpisode.fromJson(e.cast<String, dynamic>()))
              .toList()
          : const [],
      recording: j['recording'] != false,
    );
  }
}

class AlertEvent {
  const AlertEvent(
      {required this.level, required this.reason, required this.at});
  final String level;
  final String reason;
  final DateTime at;

  factory AlertEvent.fromJson(Map<String, dynamic> j) => AlertEvent(
        level: j['level'] as String? ?? 'alert',
        reason: j['reason'] as String? ?? '',
        at: fromEpoch(_num(j['ts'])) ?? DateTime.now(),
      );
}
