import 'package:flutter/material.dart';

import '../data/store.dart';
import '../theme.dart';

/// 호흡 칸에 보여 줄 내용. 판정 규칙(2026-11-05 결정)을 한 곳에 둔다.
///   레이더 메시지가 끊김           → "연결 확인"(화면에만 알림, 진동 없음)
///   무호흡(5회/분 이하 20초 이상)  → 경보
///   5회/분 이하가 이어지는 중      → 0~10초 차분한 색, 10초부터 경고 색으로 경과 시간
///   대상 없음(카운트 중 아님)      → "감지 안 됨"
///   움직임·거리로 못 잼            → 서비스가 8초 동안 직전 값을 유지하고, 그 뒤 "확인 중"
class BreathView {
  const BreathView(this.value, this.unit, this.note, this.color,
      {this.number = false});
  final String value, unit, note;
  final Color color;
  final bool number;

  static const _calmUntilS = 10;

  factory BreathView.of(NuniStore store) {
    final r = store.radar;
    if (store.link == Link.ok) {
      if (r == null || !store.radarFresh) {
        return const BreathView(
            '연결 확인', '', '레이더 연결 확인이 필요해요', Palette.mist);
      }
      if (r.apnea) {
        return BreathView('무호흡 의심', '',
            '호흡이 ${r.apneaHoldS.round()}초 넘게 약하게 잡혀요', Palette.alarm);
      }
      final low = r.lowBreathS;
      if (low != null) {
        final s = low.floor();
        return s < _calmUntilS
            ? BreathView('다시 확인 중', '$s초', '호흡 신호를 다시 확인하고 있어요', Palette.moon)
            : BreathView('약하게 잡힘', '$s초', '호흡이 약하게 잡혀요', Palette.lamp);
      }
      if (!r.presence) {
        return const BreathView('감지 안 됨', '', '아기가 감지되지 않아요', Palette.mist);
      }
    }
    if (store.breathObserved) {
      return BreathView('${r!.breathingRate!.round()}', '회/분', '센서가 확인한 현재 호흡',
          Palette.moon,
          number: true);
    }
    return const BreathView('확인 중', '', '유효한 호흡 신호를 기다려요', Palette.moon);
  }
}
