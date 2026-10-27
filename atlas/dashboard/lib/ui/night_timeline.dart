import 'dart:math';

import 'package:flutter/material.dart';

import '../data/models.dart';
import '../data/store.dart';
import '../theme.dart';

/// 오늘 밤(19시 ~ 다음 날 7시) 사건 줄 — 울음(답에 따라 색), 수유, 경보, 지금.
/// 낮에는 최근 12시간을 보인다.
class NightTimeline extends StatelessWidget {
  const NightTimeline({super.key, required this.store});
  final NuniStore store;

  (DateTime, DateTime, String) _window(DateTime now) {
    if (now.hour >= 19) {
      final s = DateTime(now.year, now.month, now.day, 19);
      return (s, s.add(const Duration(hours: 12)), '오늘 밤');
    }
    if (now.hour < 7) {
      final e = DateTime(now.year, now.month, now.day, 7);
      return (e.subtract(const Duration(hours: 12)), e, '오늘 밤');
    }
    return (now.subtract(const Duration(hours: 12)), now, '최근 12시간');
  }

  @override
  Widget build(BuildContext context) {
    final now = DateTime.now();
    final (start, end, title) = _window(now);
    final log = store.cry;
    final eps = log?.episodes ?? const <CryEpisode>[];
    final feeds = <DateTime>{
      for (final e in eps)
        if (e.action == 'feed') e.t1,
      if (log?.lastFeed != null) log!.lastFeed!,
    }.toList();
    final cur = log?.current;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(children: [
          SizedBox(
              width: 150,
              child: Text(title,
                  style: Typo.sans(22, weight: 520, color: Palette.mist))),
          const _Legend(),
        ]),
        const SizedBox(height: 10),
        Row(
          children: [
            const SizedBox(width: 150),
            Expanded(
              child: SizedBox(
                height: 104,
                child: CustomPaint(
                  painter: _TimelinePainter(
                    start: start,
                    end: end,
                    now: now,
                    episodes: eps,
                    current: cur,
                    feeds: feeds,
                    alerts: store.alerts,
                    label: store.label,
                  ),
                ),
              ),
            ),
          ],
        ),
      ],
    );
  }
}

class _Legend extends StatelessWidget {
  const _Legend();

  Widget _item(Widget mark, String text) => Padding(
        padding: const EdgeInsets.only(right: 30),
        child: Row(mainAxisSize: MainAxisSize.min, children: [
          mark,
          const SizedBox(width: 8),
          Text(text, style: Typo.sans(18, color: Palette.mist))
        ]),
      );

  @override
  Widget build(BuildContext context) => Row(children: [
        _item(
            Container(
                width: 26,
                height: 10,
                decoration: BoxDecoration(
                    color: Palette.breath,
                    borderRadius: BorderRadius.circular(5))),
            '돌봄 기록'),
        _item(
            Container(
                width: 26,
                height: 10,
                decoration: BoxDecoration(
                    border: Border.all(color: Palette.lamp, width: 2),
                    borderRadius: BorderRadius.circular(5))),
            '기록 대기'),
        _item(
            Container(
                width: 10,
                height: 10,
                decoration: const BoxDecoration(
                    color: Palette.moon, shape: BoxShape.circle)),
            '수유'),
        _item(
            CustomPaint(size: const Size(12, 11), painter: _TrianglePainter()),
            '확인 알림'),
      ]);
}

class _TrianglePainter extends CustomPainter {
  @override
  void paint(Canvas canvas, Size s) {
    canvas.drawPath(
        Path()
          ..moveTo(s.width / 2, 0)
          ..lineTo(s.width, s.height)
          ..lineTo(0, s.height)
          ..close(),
        Paint()..color = Palette.alarm);
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => false;
}

class _TimelinePainter extends CustomPainter {
  _TimelinePainter({
    required this.start,
    required this.end,
    required this.now,
    required this.episodes,
    required this.current,
    required this.feeds,
    required this.alerts,
    required this.label,
  });

  final DateTime start, end, now;
  final List<CryEpisode> episodes;
  final CryNow? current;
  final List<DateTime> feeds;
  final List<AlertEvent> alerts;
  final String Function(String) label;

  @override
  void paint(Canvas canvas, Size size) {
    final w = size.width;
    const axisY = 58.0;
    final total = end.difference(start).inMilliseconds.toDouble();
    double x(DateTime t) =>
        (t.difference(start).inMilliseconds / total).clamp(0.0, 1.0) * w;
    bool inside(DateTime t) => !t.isBefore(start) && !t.isAfter(end);

    // 축과 시각
    canvas.drawLine(
        const Offset(0, axisY),
        Offset(w, axisY),
        Paint()
          ..color = Palette.line
          ..strokeWidth = 2);
    var h = DateTime(start.year, start.month, start.day, start.hour);
    if (h.isBefore(start)) h = h.add(const Duration(hours: 1));
    for (; !h.isAfter(end); h = h.add(const Duration(hours: 1))) {
      final hx = x(h);
      canvas.drawLine(
          Offset(hx, axisY - 6),
          Offset(hx, axisY + 6),
          Paint()
            ..color = Palette.line
            ..strokeWidth = 2);
      _text(canvas, '${h.hour}', Offset(hx, axisY + 14),
          Typo.sans(18, color: Palette.mist, tabular: true),
          center: true);
    }

    // 울음 구간: 답한 것은 청자색, 답 없는 것은 수유등 테두리. 답 이름은 위에 작게(서로 겹치거나 '지금'과 겹치면 생략)
    double lastLabelEnd = -1e9;
    final nowX = x(now);
    void bar(DateTime t0, DateTime t1, String? action, {bool live = false}) {
      if (t1.isBefore(start) || t0.isAfter(end)) return;
      final x0 = x(t0), x1 = max(x(t1), x0 + 10);
      final r = RRect.fromRectAndRadius(
          Rect.fromLTRB(x0, axisY - 9, x1, axisY + 9),
          const Radius.circular(9));
      if (action != null && !live) {
        canvas.drawRRect(r, Paint()..color = Palette.breath);
        final text = label(action);
        final tp =
            _layout(text, Typo.sans(17, weight: 520, color: Palette.moonDim));
        final lx = x0 + (x1 - x0) / 2 - tp.width / 2;
        final nearNow = lx < nowX + 30 && lx + tp.width > nowX - 30;
        if (lx > lastLabelEnd + 12 && !nearNow) {
          tp.paint(canvas, Offset(lx, axisY - 40));
          lastLabelEnd = lx + tp.width;
        }
      } else {
        canvas.drawRRect(
            r,
            Paint()
              ..color = live ? Palette.lamp : Palette.ink
              ..style = live ? PaintingStyle.fill : PaintingStyle.fill);
        if (!live) {
          canvas.drawRRect(
              r,
              Paint()
                ..color = Palette.lamp
                ..style = PaintingStyle.stroke
                ..strokeWidth = 2.5);
        }
      }
    }

    for (final e in episodes) {
      bar(e.t0, e.t1, e.action);
    }
    if (current != null) bar(current!.t0, now, null, live: true);

    // 수유
    for (final f in feeds) {
      if (inside(f))
        canvas.drawCircle(
            Offset(x(f), axisY + 0.5), 7, Paint()..color = Palette.moon);
    }
    // 경보
    for (final a in alerts) {
      if (!inside(a.at) || a.level != 'alert') continue;
      final ax = x(a.at);
      canvas.drawPath(
          Path()
            ..moveTo(ax, axisY - 34)
            ..lineTo(ax + 8, axisY - 20)
            ..lineTo(ax - 8, axisY - 20)
            ..close(),
          Paint()..color = Palette.alarm);
    }
    // 지금
    if (inside(now) || now.isAfter(end)) {
      final nx = x(now);
      canvas.drawLine(
          Offset(nx, axisY - 30),
          Offset(nx, axisY + 8),
          Paint()
            ..color = Palette.moon
            ..strokeWidth = 2);
      _text(canvas, '지금', Offset(nx, axisY - 50), Typo.sans(17, weight: 560),
          center: true, clampTo: w);
    }
  }

  TextPainter _layout(String s, TextStyle st) => TextPainter(
      text: TextSpan(text: s, style: st), textDirection: TextDirection.ltr)
    ..layout();

  void _text(Canvas c, String s, Offset at, TextStyle st,
      {bool center = false, double? clampTo}) {
    final tp = _layout(s, st);
    var dx = center ? at.dx - tp.width / 2 : at.dx;
    if (clampTo != null) dx = dx.clamp(0, clampTo - tp.width);
    tp.paint(c, Offset(dx, at.dy));
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => true;
}
