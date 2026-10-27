import 'dart:math';

import 'package:flutter/material.dart';

import '../theme.dart';

enum Tone { answer, quiet, alarm }

/// 마우스·키보드 버튼. 단축키가 있으면 왼쪽에 키 표시. 키보드 초점은 상아색 테두리로 보인다.
class NuniButton extends StatelessWidget {
  const NuniButton({
    super.key,
    required this.label,
    required this.onPressed,
    this.keyHint,
    this.tone = Tone.answer,
    this.size = 26,
    this.expand = false,
    this.focusNode,
    this.autofocus = false,
  });

  final String label;
  final VoidCallback? onPressed;
  final String? keyHint;
  final Tone tone;
  final double size;
  final bool expand;
  final FocusNode? focusNode;
  final bool autofocus;

  @override
  Widget build(BuildContext context) {
    final fg = switch (tone) {
      Tone.alarm => Palette.ink,
      Tone.quiet => Palette.mist,
      Tone.answer => Palette.moon
    };
    final bg = switch (tone) {
      Tone.alarm => Palette.alarm,
      Tone.quiet => Colors.transparent,
      Tone.answer => Palette.button
    };
    final pad = tone == Tone.quiet
        ? const EdgeInsets.symmetric(horizontal: 6, vertical: 10)
        : EdgeInsets.symmetric(horizontal: size * 0.9, vertical: size * 0.62);
    final child = Row(
      mainAxisSize: expand ? MainAxisSize.max : MainAxisSize.min,
      children: [
        if (keyHint != null) ...[
          KeyCap(keyHint!, dark: tone == Tone.alarm),
          SizedBox(width: size * 0.55)
        ],
        Flexible(
            child: Text(label,
                style: Typo.sans(size,
                    weight: tone == Tone.quiet ? 450 : 560,
                    color: fg,
                    height: 1.2))),
      ],
    );
    return TextButton(
      onPressed: onPressed,
      focusNode: focusNode,
      autofocus: autofocus,
      style: ButtonStyle(
        padding: WidgetStatePropertyAll(pad),
        alignment: Alignment.centerLeft,
        backgroundColor: WidgetStateProperty.resolveWith((s) {
          if (tone == Tone.quiet) return Colors.transparent;
          if (s.contains(WidgetState.hovered) ||
              s.contains(WidgetState.focused)) {
            return tone == Tone.alarm
                ? const Color(0xFFA33432)
                : Palette.buttonHover;
          }
          return bg;
        }),
        overlayColor: const WidgetStatePropertyAll(Colors.transparent),
        shape: WidgetStateProperty.resolveWith((s) => RoundedRectangleBorder(
              borderRadius: BorderRadius.circular(14),
              side: s.contains(WidgetState.focused)
                  ? const BorderSide(color: Palette.moon, width: 2.5)
                  : BorderSide(
                      color: tone == Tone.answer
                          ? Palette.line
                          : Colors.transparent,
                      width: 1.5),
            )),
        foregroundColor: WidgetStateProperty.resolveWith((s) =>
            tone == Tone.quiet &&
                    (s.contains(WidgetState.hovered) ||
                        s.contains(WidgetState.focused))
                ? Palette.moon
                : fg),
        mouseCursor: const WidgetStatePropertyAll(SystemMouseCursors.click),
      ),
      child: child,
    );
  }
}

/// 키보드 단축키 표시
class KeyCap extends StatelessWidget {
  const KeyCap(this.text, {super.key, this.dark = false});
  final String text;
  final bool dark;

  @override
  Widget build(BuildContext context) => Container(
        constraints: const BoxConstraints(minWidth: 34),
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
        decoration: BoxDecoration(
          borderRadius: BorderRadius.circular(7),
          border: Border.all(
              color:
                  (dark ? Palette.ink : Palette.mist).withValues(alpha: 0.55),
              width: 1.5),
        ),
        alignment: Alignment.center,
        child: Text(text,
            style: Typo.sans(17,
                weight: 600,
                color: dark ? Palette.ink : Palette.mist,
                height: 1.3)),
      );
}

/// 답이 힌트 기준(k_min)까지 몇 개 쌓였는지 — 점 하나가 답 하나
class AnswerDots extends StatelessWidget {
  const AnswerDots({super.key, required this.done, required this.total});
  final int done, total;

  @override
  Widget build(BuildContext context) => Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          for (var i = 0; i < total; i++)
            Container(
              width: 18,
              height: 18,
              margin: const EdgeInsets.only(right: 10),
              decoration: BoxDecoration(
                shape: BoxShape.circle,
                color: i < done ? Palette.breath : Colors.transparent,
                border: Border.all(
                    color: i < done ? Palette.breath : Palette.line, width: 2),
              ),
            ),
        ],
      );
}

/// 최근 호흡수 작은 선(10분)
class Sparkline extends StatelessWidget {
  const Sparkline(
      {super.key, required this.points, this.width = 220, this.height = 44});
  final List<(DateTime, double)> points;
  final double width, height;

  @override
  Widget build(BuildContext context) => CustomPaint(
      size: Size(width, height),
      painter: _SparkPainter(points, DateTime.now()));
}

class _SparkPainter extends CustomPainter {
  _SparkPainter(this.points, this.now);
  final List<(DateTime, double)> points;
  final DateTime now;

  @override
  void paint(Canvas canvas, Size size) {
    const window = Duration(minutes: 10);
    final pts = points.where((p) => now.difference(p.$1) <= window).toList();
    final axis = Paint()
      ..color = Palette.line
      ..strokeWidth = 1.5;
    canvas.drawLine(
        Offset(0, size.height - 1), Offset(size.width, size.height - 1), axis);
    if (pts.length < 2) return;
    final lo = pts.map((p) => p.$2).reduce(min) - 2,
        hi = pts.map((p) => p.$2).reduce(max) + 2;
    Offset at((DateTime, double) p) {
      final x = size.width *
          (1 - now.difference(p.$1).inMilliseconds / window.inMilliseconds);
      final y =
          size.height - 4 - (size.height - 8) * ((p.$2 - lo) / max(hi - lo, 1));
      return Offset(x, y);
    }

    final path = Path()..moveTo(at(pts.first).dx, at(pts.first).dy);
    for (final p in pts.skip(1)) {
      final o = at(p);
      path.lineTo(o.dx, o.dy);
    }
    canvas.drawPath(
        path,
        Paint()
          ..color = Palette.mist
          ..strokeWidth = 2
          ..style = PaintingStyle.stroke);
    canvas.drawCircle(at(pts.last), 4.5, Paint()..color = Palette.breath);
  }

  @override
  bool shouldRepaint(_SparkPainter old) => true;
}
