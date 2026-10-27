import 'dart:math';

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';

import '../theme.dart';

enum HorizonMode { breathing, waiting, stopped }

/// 숨의 수평선 — 측정한 호흡수에 맞춰 이불처럼 천천히 부풀었다 가라앉는 선.
/// 흘러가는 파형(병원 모니터)이 아니라 제자리에서 오르내린다. 실제 레이더 파형이 아니라 호흡수로 그린 모양이다.
///   breathing  호흡수(회/분)로 오르내림, 움직임이 크면 잔물결
///   waiting    레이더 신호 없음 — 점선으로 가만히
///   stopped    호흡이 잡히지 않음(경보) — 경보색 직선
class BreathHorizon extends StatefulWidget {
  const BreathHorizon({super.key, required this.mode, this.rate = 30, this.movement = 0, this.reduceMotion = false});

  final HorizonMode mode;
  final double rate;
  final double movement;
  final bool reduceMotion;

  @override
  State<BreathHorizon> createState() => _BreathHorizonState();
}

class _BreathHorizonState extends State<BreathHorizon> with SingleTickerProviderStateMixin {
  late final Ticker _ticker;
  Duration _last = Duration.zero;
  double _phase = 0; // 숨 한 번 = 2π
  double _time = 0;

  @override
  void initState() {
    super.initState();
    _ticker = createTicker(_onTick)..start();
  }

  void _onTick(Duration elapsed) {
    final dt = (elapsed - _last).inMicroseconds / 1e6;
    _last = elapsed;
    if (widget.mode != HorizonMode.breathing || widget.reduceMotion) return;
    final rate = widget.rate.clamp(6, 90);
    setState(() {
      _phase += 2 * pi * rate / 60 * dt;
      _time += dt;
    });
  }

  @override
  void dispose() {
    _ticker.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => CustomPaint(
        painter: _HorizonPainter(
          mode: widget.mode,
          lift: widget.reduceMotion ? 0.5 : 0.5 - 0.5 * cos(_phase),
          time: _time,
          movement: widget.movement.clamp(0, 1).toDouble(),
        ),
        size: Size.infinite,
      );
}

class _HorizonPainter extends CustomPainter {
  _HorizonPainter({required this.mode, required this.lift, required this.time, required this.movement});

  final HorizonMode mode;
  final double lift; // 0(내쉼) ~ 1(들숨)
  final double time;
  final double movement;

  @override
  void paint(Canvas canvas, Size size) {
    final w = size.width, h = size.height;
    final base = h * 0.72;
    if (mode != HorizonMode.breathing) {
      final paint = Paint()
        ..color = mode == HorizonMode.stopped ? Palette.alarm : Palette.mist.withValues(alpha: 0.45)
        ..strokeWidth = mode == HorizonMode.stopped ? 3 : 2
        ..style = PaintingStyle.stroke;
      if (mode == HorizonMode.stopped) {
        canvas.drawLine(Offset(0, base), Offset(w, base), paint);
      } else {
        for (double x = 0; x < w; x += 18) {
          canvas.drawLine(Offset(x, base), Offset(min(x + 8, w), base), paint);
        }
      }
      return;
    }

    // 세 개의 완만한 언덕(이불 아래 몸)이 천천히 자리를 옮기며 숨에 맞춰 부푼다
    final amp = h * 0.42;
    final centers = [
      w * 0.30 + w * 0.03 * sin(time * 0.11),
      w * 0.55 + w * 0.025 * sin(time * 0.07 + 1.3),
      w * 0.78 + w * 0.03 * sin(time * 0.09 + 2.1),
    ];
    const weights = [0.85, 1.0, 0.55];
    final sigma = w * 0.13;
    final path = Path();
    final fill = Path()..moveTo(0, h);
    for (double x = 0; x <= w; x += 4) {
      double g = 0;
      for (var k = 0; k < 3; k++) {
        final d = (x - centers[k]) / sigma;
        g += weights[k] * exp(-0.5 * d * d);
      }
      final ripple = movement * 7 * sin(x * 0.045 + time * 5.5) * (0.4 + 0.6 * g);
      final y = base - amp * g * (0.35 + 0.65 * lift) - ripple;
      if (x == 0) {
        path.moveTo(x, y);
      } else {
        path.lineTo(x, y);
      }
      fill.lineTo(x, y);
    }
    fill
      ..lineTo(w, h)
      ..close();
    canvas.drawPath(
      fill,
      Paint()
        ..shader = LinearGradient(
          begin: Alignment.topCenter,
          end: Alignment.bottomCenter,
          colors: [Palette.breath.withValues(alpha: 0.13), Palette.breath.withValues(alpha: 0.0)],
        ).createShader(Rect.fromLTWH(0, 0, w, h)),
    );
    canvas.drawPath(
      path,
      Paint()
        ..color = Palette.breath
        ..strokeWidth = 2.6
        ..style = PaintingStyle.stroke
        ..strokeJoin = StrokeJoin.round,
    );
  }

  @override
  bool shouldRepaint(_HorizonPainter old) =>
      old.lift != lift || old.time != time || old.mode != mode || old.movement != movement;
}
