import 'package:flutter/material.dart';

/// 검은 펭귄을 중심으로 한 아이보리·세이지 팔레트.
class Palette {
  static const ink = Color(0xFFF6F5EF);
  static const inkRaised = Color(0xFFFFFFFF);
  static const line = Color(0xFFE0E5DD);
  static const moon = Color(0xFF28332F);
  static const moonDim = Color(0xFF536359);
  static const mist = Color(0xFF637168);
  static const breath = Color(0xFF286B57);
  static const lamp = Color(0xFF926019);
  static const alarm = Color(0xFFB93E3C);
  static const sage = Color(0xFFE9F1E9);
  static const peach = Color(0xFFFBEEE0);
  static const rose = Color(0xFFFAE8E5);
  static const button = Color(0xFFEDF3ED);
  static const buttonHover = Color(0xFFDEEBDD);
}

/// 화면을 통째로 축소하지 않고 작은 화면에서도 실제 논리 픽셀 크기를 유지한다.
/// 글자 크기 단계: 16 · 18 · 22 · 26 · 30 · 40 · 56 · 84
class Typo {
  /// 명조 — 기기가 부모에게 건네는 말(상태 문장·질문)
  static TextStyle serif(double size,
          {double weight = 500,
          Color color = Palette.moon,
          double height = 1.22}) =>
      TextStyle(
        fontFamily: 'NotoSerifKR',
        fontSize: size,
        height: height,
        color: color,
        fontWeight: _weight(weight),
        fontVariations: [FontVariation('wght', weight)],
        letterSpacing: size >= 40 ? -0.012 * size : 0,
      );

  /// 고딕 — 화면 글자·숫자
  static TextStyle sans(double size,
          {double weight = 420,
          Color color = Palette.moon,
          double height = 1.38,
          bool tabular = false}) =>
      TextStyle(
        fontFamily: 'NotoSansKR',
        fontSize: size,
        height: height,
        color: color,
        fontWeight: _weight(weight),
        fontVariations: [FontVariation('wght', weight)],
        fontFeatures: tabular ? const [FontFeature.tabularFigures()] : null,
        letterSpacing: size >= 40 ? -0.01 * size : 0,
      );

  static FontWeight _weight(double w) {
    final i = ((w / 100).round().clamp(1, 9)) - 1;
    return FontWeight.values[i];
  }
}

ThemeData nuniTheme() => ThemeData(
      brightness: Brightness.light,
      scaffoldBackgroundColor: Palette.ink,
      fontFamily: 'NotoSansKR',
      colorScheme: const ColorScheme.light(
        surface: Palette.inkRaised,
        primary: Palette.breath,
        secondary: Palette.lamp,
        error: Palette.alarm,
        onSurface: Palette.moon,
      ),
      focusColor: Palette.moon.withValues(alpha: 0.16),
      hoverColor: Palette.moon.withValues(alpha: 0.06),
      splashFactory: NoSplash.splashFactory,
    );
