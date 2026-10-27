import 'dart:async';
import 'dart:convert';

import 'package:dbus/dbus.dart';

import '../copy.dart';
import 'models.dart';
import 'store.dart';

/// 기기에서 쓰는 저장소 — com.atlas.NuniFusion1(D-Bus, 시스템 버스)
///   신호: StateChanged(융합 상태) · Alert · CryEpisode(울음 시작·끝·답)
///   주기 호출: GetSensors 1초, GetCryLog·GetAlerts 4초(울음 사건이 오면 바로)
/// 서비스가 내려가 있거나 다시 켜지면 호출이 실패하다가 다시 붙는다(신호 구독은 이름 기준이라 그대로 유효).
class DbusStore extends NuniStore {
  static const _name = 'com.atlas.NuniFusion1';
  static const _path = '/com/atlas/NuniFusion1';
  static const _iface = 'com.atlas.NuniFusion1';

  final _client = DBusClient.system();
  late final _obj = DBusRemoteObject(_client, name: _name, path: DBusObjectPath(_path));
  final _subs = <StreamSubscription>[];
  Timer? _fast, _slow;
  DateTime _lastStateSignal = DateTime.fromMillisecondsSinceEpoch(0);
  bool _slowBusy = false;

  @override
  void start() {
    _subs.add(_signal('StateChanged').listen((s) {
      _lastStateSignal = DateTime.now();
      _applyState(s.values.first.asString());
    }));
    _subs.add(_signal('Alert').listen((s) => _pollSlow()));
    _subs.add(_signal('CryEpisode').listen((s) => _pollSlow()));
    _fast = Timer.periodic(const Duration(seconds: 1), (_) => _pollFast());
    _slow = Timer.periodic(const Duration(seconds: 4), (_) => _pollSlow());
    _pollFast();
    _pollSlow();
  }

  Stream<DBusSignal> _signal(String name) =>
      DBusRemoteObjectSignalStream(object: _obj, interface: _iface, name: name);

  Future<String> _call(String method, [List<DBusValue> args = const []]) async {
    final r = await _obj.callMethod(_iface, method, args, replySignature: DBusSignature('s'));
    return r.values.first.asString();
  }

  void _applyState(String raw) {
    try {
      state = FusionState.fromJson((jsonDecode(raw) as Map).cast<String, dynamic>());
      notifyListeners();
    } catch (_) {}
  }

  Future<void> _pollFast() async {
    try {
      final s = Sensors.fromJson((jsonDecode(await _call('GetSensors')) as Map).cast<String, dynamic>());
      sensors = s;
      if (s.radar != null) recordBreath(s.radar!);
      // 상태 신호가 한동안 없으면(서비스 재시작 직후 등) 직접 읽는다
      if (DateTime.now().difference(_lastStateSignal).inSeconds > 3) _applyState(await _call('GetState'));
      _setLink(Link.ok, null);
    } catch (e) {
      _setLink(Link.lost, _describe(e));
    }
  }

  Future<void> _pollSlow() async {
    if (_slowBusy) return;
    _slowBusy = true;
    try {
      cry = CryLogView.fromJson((jsonDecode(await _call('GetCryLog')) as Map).cast<String, dynamic>());
      final a = jsonDecode(await _call('GetAlerts'));
      alerts = a is List ? a.whereType<Map>().map((e) => AlertEvent.fromJson(e.cast<String, dynamic>())).toList() : const [];
      notifyListeners();
    } catch (_) {
      // 연결 상태는 _pollFast가 정한다
    } finally {
      _slowBusy = false;
    }
  }

  void _setLink(Link l, String? err) {
    if (link == l && linkError == err) {
      notifyListeners();
      return;
    }
    link = l;
    linkError = err;
    notifyListeners();
  }

  String _describe(Object e) {
    final s = '$e';
    if (s.contains('ServiceUnknown') || s.contains('NameHasNoOwner')) return '센서 서비스가 꺼져 있어요';
    if (s.contains('AccessDenied')) return '센서 서비스에 접근할 권한이 없어요';
    return '센서 서비스 응답이 없어요';
  }

  Future<String?> _act(String method, List<DBusValue> args, String okText) async {
    try {
      final r = (jsonDecode(await _call(method, args)) as Map).cast<String, dynamic>();
      if (r['ok'] == true) {
        showNotice(okText);
        await _pollSlow();
        return null;
      }
      final err = r['error'] as String? ?? '기록하지 못했어요';
      showNotice(err, error: true);
      return err;
    } catch (e) {
      final err = '${_describe(e)}. 답을 기록하지 못했어요';
      showNotice(err, error: true);
      return err;
    }
  }

  @override
  Future<String?> answer(String id, String action) =>
      _act('AnswerCry', [DBusString(id), DBusString(action)], '기록했어요: ${label(action)}');

  @override
  Future<String?> answerNot(String id, String action) =>
      _act('AnswerCryNot', [DBusString(id), DBusString(action)], '기록했어요: ${eunNeun(label(action))} 아니었어요');

  @override
  Future<String?> logFeed() => _act('LogFeed', const [], '수유를 기록했어요');

  @override
  void dispose() {
    for (final s in _subs) {
      s.cancel();
    }
    _fast?.cancel();
    _slow?.cancel();
    _client.close();
    super.dispose();
  }
}
