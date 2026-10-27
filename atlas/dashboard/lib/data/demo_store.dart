import 'dart:async';
import 'dart:math';

import '../copy.dart';
import 'models.dart';
import 'store.dart';

/// 시연·미리보기용 가상 데이터(센서 없이 화면을 볼 때). 빌드할 때 --dart-define=NUNI_DEMO=<장면>
///   calm      편안하게 자는 밤(답 11번 쌓임)
///   crying    지금 우는 중, 1·2순위 힌트가 보이는 상태
///   learning  지금 우는 중, 답이 아직 적어 힌트를 숨긴 상태(답 4번)
///   question  울음이 막 끝나 "무엇으로 그쳤나요?"를 묻는 상태
///   alert     호흡이 잡히지 않는 경보
///   live      평온 → 울음 → 질문을 되풀이(답하면 다시 평온)
/// 숫자는 화면 확인용 예시이고 실제 측정값이 아니다.
class DemoStore extends NuniStore {
  DemoStore(this.scene);

  final String scene;
  final _rng = Random(7);
  final _eps = <_Ep>[];
  DateTime? _lastFeed;
  _Cur? _cur;
  Timer? _timer;
  int _tick = 0;
  int _seq = 0;

  @override
  bool get isDemo => true;

  @override
  void start() {
    _seed();
    _step();
    _timer = Timer.periodic(const Duration(seconds: 1), (_) => _step());
  }

  String _id(DateTime t) => 'demo-${t.millisecondsSinceEpoch}-${_seq++}';

  void _seed() {
    final now = DateTime.now();
    final answers = scene == 'learning'
        ? ['feed', 'diaper', 'hold', 'feed']
        : ['feed', 'hold', 'diaper', 'feed', 'diaper', 'feed', 'self', 'hold', 'feed', 'diaper', 'feed'];
    var t = now.subtract(Duration(minutes: 40 * answers.length + 25));
    for (final a in answers) {
      final len = Duration(seconds: 60 + _rng.nextInt(260));
      _eps.add(_Ep(_id(t), t, t.add(len), a, hint: const ['feed', 'diaper']));
      t = t.add(Duration(minutes: 30 + _rng.nextInt(20)));
    }
    _lastFeed = now.subtract(const Duration(minutes: 132));
    if (scene == 'crying' || scene == 'learning') _cur = _Cur(_id(now), now.subtract(const Duration(seconds: 72)));
    if (scene == 'question') {
      final t0 = now.subtract(const Duration(minutes: 4));
      _eps.add(_Ep(_id(t0), t0, now.subtract(const Duration(seconds: 40)), null));
    }
    alerts = [
      AlertEvent(level: 'attention', reason: '울음 감지(none)', at: now.subtract(const Duration(hours: 3, minutes: 12))),
    ];
  }

  int get _labeled => _eps.where((e) => e.action == 'feed' || e.action == 'diaper' || e.action == 'hold').length;

  void _step() {
    _tick++;
    final now = DateTime.now();
    if (scene == 'live') _live(now);
    final crying = _cur != null;
    final t = _tick.toDouble();
    final alert = scene == 'alert';
    final br = alert ? 0.0 : (crying ? 44 + 3 * sin(t / 5) : 31 + 2 * sin(t / 40) + _rng.nextDouble() - 0.5);
    final mv = alert ? 0.0 : (crying ? 0.72 + 0.1 * _rng.nextDouble() : 0.06 + 0.05 * _rng.nextDouble());
    final ts = now.millisecondsSinceEpoch / 1000;
    final radar = Radar(
        breathingRate: br, movement: mv, presence: true, apnea: alert, breathValid: true, distance: 0.62, ts: ts);
    sensors = Sensors(
      radar: radar,
      cry: CrySound(isCrying: crying, confidence: crying ? 0.86 : 0.02, ts: ts),
      room: Room(temp: 22.6, humidity: 38, co2: 640, lux: 3, ts: ts),
    );
    recordBreath(radar);
    state = FusionState(
      level: alert ? 'alert' : (crying ? 'attention' : 'normal'),
      reasons: alert ? const ['호흡 미검출·무호흡 의심'] : (crying ? const ['울음 감지(none)'] : const []),
      sleepState: alert ? 'hold' : (crying ? 'awake' : 'calm_sleep'),
      sleepReason: alert ? '호흡 이상 감지로 수면 판정 보류' : '',
      actions: const ['humidify'],
      ts: ts,
    );
    link = Link.ok;
    cry = _view(now);
    notifyListeners();
  }

  void _live(DateTime now) {
    final pending = _eps.isNotEmpty && _eps.last.action == null;
    if (_cur == null && !pending && _tick % 75 == 40) _cur = _Cur(_id(now), now);
    if (_cur != null && now.difference(_cur!.t0).inSeconds >= 35) {
      _eps.add(_Ep(_cur!.id, _cur!.t0, now, null, hint: List.of(_hint().top2), not: _cur!.not));
      _cur = null;
    }
  }

  CryHint _hint() {
    final show = _labeled >= 10;
    return CryHint(
      top2: const ['feed', 'diaper'],
      probs: const {'feed': 0.52, 'diaper': 0.31, 'hold': 0.17},
      show: show,
      show2: show,
      nFeedback: _labeled,
      hoursSinceFeed: _lastFeed == null ? null : DateTime.now().difference(_lastFeed!).inMinutes / 60,
      notTried: _cur?.not ?? const [],
    );
  }

  CryLogView _view(DateTime now) {
    final labeled = _eps.where((e) => e.action == 'feed' || e.action == 'diaper' || e.action == 'hold').toList();
    final pending = _eps.isNotEmpty && _eps.last.action == null ? _eps.last.id : null;
    final recent = labeled.reversed.take(20).where((e) => e.hint.isNotEmpty).toList();
    return CryLogView(
      labels: const {
        'feed': '수유',
        'diaper': '기저귀',
        'hold': '안아주기·자세 바꾸기',
        'self': '저절로 그침',
        'sick': '아파 보임',
        'other': '기타',
        'unknown': '모름',
      },
      kMin: 10,
      nLabeled: labeled.length,
      nAnswered: _eps.where((e) => e.action != null).length,
      lastFeed: _lastFeed,
      current: _cur == null
          ? null
          : CryNow(id: _cur!.id, t0: _cur!.t0, seconds: now.difference(_cur!.t0).inSeconds.toDouble(), hint: _hint()),
      pending: _cur == null ? pending : null,
      hitsN: recent.length,
      hitsTop1: recent.where((e) => e.hint.first == e.action).length,
      hitsTop2: recent.where((e) => e.hint.contains(e.action)).length,
      episodes: [
        for (final e in _eps)
          CryEpisode(id: e.id, t0: e.t0, t1: e.t1, n: e.t1.difference(e.t0).inSeconds, action: e.action, notTried: e.not, hintTop2: e.hint),
      ],
    );
  }

  _Ep? _find(String id) {
    for (final e in _eps) {
      if (e.id == id) return e;
    }
    return null;
  }

  @override
  Future<String?> answer(String id, String action) async {
    final e = _find(id);
    if (e == null) {
      showNotice('그 울음 기록이 없어요', error: true);
      return '없음';
    }
    e.action = action;
    if (action == 'feed') _lastFeed = e.t1;
    showNotice('기록했어요: ${label(action)}');
    _step();
    return null;
  }

  @override
  Future<String?> answerNot(String id, String action) async {
    final list = _cur?.id == id ? _cur!.not : _find(id)?.not;
    if (list != null && !list.contains(action)) list.add(action);
    showNotice('기록했어요: ${eunNeun(label(action))} 아니었어요');
    _step();
    return null;
  }

  @override
  Future<String?> logFeed() async {
    _lastFeed = DateTime.now();
    showNotice('수유를 기록했어요');
    _step();
    return null;
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }
}

class _Ep {
  _Ep(this.id, this.t0, this.t1, this.action, {this.hint = const [], List<String>? not}) : not = not ?? [];
  final String id;
  final DateTime t0, t1;
  String? action;
  final List<String> hint;
  final List<String> not;
}

class _Cur {
  _Cur(this.id, this.t0);
  final String id;
  final DateTime t0;
  final List<String> not = [];
}
