import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import '../copy.dart';
import '../data/store.dart';
import '../data/care_history.dart';
import '../theme.dart';
import 'common.dart';
import 'current_panel.dart';
import 'history_panel.dart';

class Dashboard extends StatefulWidget {
  const Dashboard({super.key, required this.store, this.historyPath});
  final NuniStore store;
  final String? historyPath;
  @override
  State<Dashboard> createState() => _DashboardState();
}

class _DashboardState extends State<Dashboard> {
  late final CareHistory _history;
  late final HistoryView _examples;
  final _focus = FocusNode();
  bool _dim = false, _ack = false, _calmMotion = false, _past = false;
  bool _sample =
      const bool.fromEnvironment('NUNI_SAMPLE_HISTORY', defaultValue: true);
  String? _asking;
  NuniStore get s => widget.store;
  @override
  void initState() {
    super.initState();
    _examples = sampleHistory(DateTime.now());
    _history = CareHistory(s, filePath: widget.historyPath)
      ..addListener(_refresh);
  }

  void _refresh() {
    if (s.state.level != 'alert') _ack = false;
    if (_asking != null && s.cry?.episode(_asking)?.action != null)
      _asking = null;
    if (mounted) setState(() {});
  }

  @override
  void dispose() {
    _history.removeListener(_refresh);
    _history.dispose();
    _focus.dispose();
    super.dispose();
  }

  KeyEventResult _onKey(FocusNode node, KeyEvent event) {
    if (event is! KeyDownEvent) return KeyEventResult.ignored;
    final k = event.logicalKey;
    if (k == LogicalKeyboardKey.keyD) {
      setState(() => _dim = !_dim);
      return KeyEventResult.handled;
    }
    if (k == LogicalKeyboardKey.keyM) {
      setState(() => _calmMotion = !_calmMotion);
      return KeyEventResult.handled;
    }
    if (k == LogicalKeyboardKey.keyF && s.link == Link.ok) {
      s.logFeed();
      return KeyEventResult.handled;
    }
    if (k == LogicalKeyboardKey.enter && s.state.level == 'alert') {
      setState(() => _ack = true);
      return KeyEventResult.handled;
    }
    final id = _asking ??
        (s.soundAvailable ? s.cry?.current?.id : null) ??
        s.cry?.pending;
    final digit = [
      LogicalKeyboardKey.digit1,
      LogicalKeyboardKey.digit2,
      LogicalKeyboardKey.digit3
    ].indexOf(k);
    if (digit >= 0 && id != null && s.link == Link.ok) {
      _history.respond(id, const ['feed', 'diaper', 'hold'][digit]);
      return KeyEventResult.handled;
    }
    return KeyEventResult.ignored;
  }

  @override
  Widget build(BuildContext context) {
    final alert = s.link == Link.ok && s.state.level == 'alert' && !_ack;
    final current = CurrentPanel(
        store: s,
        history: _history,
        recordId: _asking,
        acknowledged: _ack,
        onAcknowledge: () => setState(() => _ack = true));
    final history = HistoryPanel(
        view: _sample ? _examples : _history.real,
        store: s,
        onSample: (value) => setState(() => _sample = value),
        onRecord: (r) => setState(() {
              _asking = r.id;
              _past = false;
            }));
    return Focus(
        focusNode: _focus,
        autofocus: true,
        onKeyEvent: _onKey,
        child: MediaQuery(
            data: MediaQuery.of(context).copyWith(
                disableAnimations:
                    _calmMotion || MediaQuery.of(context).disableAnimations),
            child: LayoutBuilder(builder: (context, constraints) {
              final compact =
                  constraints.maxWidth < 1400 || constraints.maxHeight < 850;
              return Stack(children: [
                Padding(
                    padding: compact
                        ? const EdgeInsets.fromLTRB(20, 16, 20, 16)
                        : const EdgeInsets.fromLTRB(48, 32, 48, 24),
                    child: Column(children: [
                      _Header(
                          store: s,
                          dim: _dim,
                          compact: compact,
                          onDim: () => setState(() => _dim = !_dim)),
                      SizedBox(height: compact ? 12 : 24),
                      if (compact) ...[
                        Align(
                            alignment: Alignment.centerLeft,
                            child: SegmentedButton<bool>(
                                key: const ValueKey('page-switch'),
                                segments: const [
                                  ButtonSegment(
                                      value: false, label: Text('현재 상태')),
                                  ButtonSegment(
                                      value: true, label: Text('과거 기록'))
                                ],
                                selected: {_past},
                                showSelectedIcon: false,
                                style: SegmentedButton.styleFrom(
                                    textStyle: Typo.sans(18, weight: 600),
                                    minimumSize: const Size(130, 44),
                                    selectedBackgroundColor: Palette.sage),
                                onSelectionChanged: (values) =>
                                    setState(() => _past = values.first))),
                        const SizedBox(height: 12),
                        Expanded(
                            child: _past
                                ? SingleChildScrollView(
                                    child:
                                        SizedBox(height: 944, child: history))
                                : CurrentPanel(
                                    store: s,
                                    history: _history,
                                    recordId: _asking,
                                    compact: true,
                                    acknowledged: _ack,
                                    onAcknowledge: () =>
                                        setState(() => _ack = true))),
                      ] else
                        Expanded(
                            child: Row(
                                crossAxisAlignment: CrossAxisAlignment.stretch,
                                children: [
                              Expanded(flex: 11, child: current),
                              const SizedBox(width: 32),
                              Container(width: 1, color: Palette.line),
                              const SizedBox(width: 32),
                              Expanded(flex: 10, child: history),
                            ])),
                    ])),
                if (alert)
                  Positioned.fill(
                      child: IgnorePointer(
                          child: DecoratedBox(
                              decoration: BoxDecoration(
                                  border: Border.all(
                                      color: Palette.alarm, width: 8))))),
                if (_dim)
                  Positioned.fill(
                      child: IgnorePointer(
                          child: ColoredBox(
                              color: Colors.black.withValues(alpha: .55)))),
              ]);
            })));
  }
}

class _Header extends StatelessWidget {
  const _Header(
      {required this.store,
      required this.dim,
      required this.onDim,
      this.compact = false});
  final NuniStore store;
  final bool dim;
  final bool compact;
  final VoidCallback onDim;

  @override
  Widget build(BuildContext context) {
    final receiving = [
      if (store.breathObserved) '호흡',
      if (store.soundAvailable) '소리',
      if (store.roomAvailable) '환경'
    ];
    final (dot, text) = switch (store.link) {
      Link.ok => (
          receiving.isEmpty ? Palette.mist : Palette.breath,
          store.isDemo
              ? '미리보기 · 실제 측정값이 아니에요'
              : receiving.isEmpty
                  ? '새 센서 정보를 기다려요'
                  : '${receiving.join(' · ')} 수신 중'
        ),
      Link.connecting => (Palette.mist, '연결하는 중'),
      Link.lost => (Palette.alarm, store.linkError ?? '연결이 끊겼어요'),
    };
    return SizedBox(
      height: compact ? 44 : 56,
      child: Row(
        children: [
          Container(
            width: 44,
            height: 44,
            decoration: BoxDecoration(
              color: Palette.sage,
              borderRadius: BorderRadius.circular(15),
            ),
            child: const Icon(Icons.favorite_rounded,
                color: Palette.breath, size: 23),
          ),
          const SizedBox(width: 12),
          Text('NUNI',
              style: Typo.sans(compact ? 28 : 36, weight: 750)
                  .copyWith(letterSpacing: 2)),
          const Spacer(),
          Container(
              width: 12,
              height: 12,
              decoration: BoxDecoration(color: dot, shape: BoxShape.circle)),
          const SizedBox(width: 12),
          Text(text, style: Typo.sans(compact ? 16 : 20, color: Palette.mist)),
          SizedBox(width: compact ? 20 : 40),
          NuniButton(
              label: dim ? '화면 밝게' : '화면 어둡게',
              keyHint: 'D',
              tone: Tone.quiet,
              size: compact ? 16 : 20,
              onPressed: onDim),
          SizedBox(width: compact ? 20 : 40),
          Text(clock(DateTime.now()),
              style: Typo.sans(compact ? 24 : 30, weight: 520, tabular: true)),
        ],
      ),
    );
  }
}
