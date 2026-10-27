import 'dart:math';
import 'package:flutter/material.dart';
import '../data/store.dart';
import '../theme.dart';
import 'common.dart';

/// 호흡·울음 관측. 전파 근거는 화면 오른쪽 아래에 표시한다.
class Vitals extends StatelessWidget {
  const Vitals({super.key, required this.store});
  final NuniStore store;
  @override
  Widget build(BuildContext context) {
    final br = store.breathObserved ? store.radar?.breathingRate : null;
    final sound = store.soundAvailable;
    final crying = sound && store.sensors.cry!.isCrying;
    final hist = store.breathHistory
        .where((p) => DateTime.now().difference(p.$1).inMinutes < 10)
        .map((p) => p.$2)
        .toList();
    return SizedBox(
        height: 224,
        child: Row(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
          Expanded(
              child: _Reading(
                  icon: Icons.favorite_border_rounded,
                  label: '호흡',
                  value: br == null ? '확인 중' : '${br.round()}',
                  unit: br == null ? null : '회/분',
                  number: br != null,
                  note: br == null ? '호흡 신호를 기다려요' : '센서가 확인한 현재 호흡수',
                  extra: br != null && hist.length >= 2
                      ? Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                              Sparkline(
                                  points: store.breathHistory,
                                  width: 270,
                                  height: 24),
                              const SizedBox(height: 4),
                              Text(
                                  '최근 10분 ${hist.reduce(min).round()}–${hist.reduce(max).round()}회',
                                  style: Typo.sans(15,
                                      color: Palette.mist, height: 1.2))
                            ])
                      : Text(
                          br == null
                              ? '신호가 잡히면 수치를 보여드려요'
                              : '호흡 기록을 조금씩 모으고 있어요',
                          style: Typo.sans(17, color: Palette.mist)))),
          const SizedBox(width: 20),
          Expanded(
              child: _Reading(
                  icon: Icons.hearing_rounded,
                  label: '울음 소리',
                  value: !sound ? '확인 중' : (crying ? '울음 감지' : '울음 없음'),
                  note: !sound
                      ? '새 소리 정보가 도착하지 않았어요'
                      : (crying
                          ? '옆의 돌봄 도우미를 확인해 주세요'
                          : '최근 소리에서 울음이 감지되지 않았어요'),
                  color: crying ? Palette.lamp : Palette.moon,
                  extra: Row(children: [
                    Icon(
                        sound
                            ? Icons.check_circle_outline_rounded
                            : Icons.schedule_rounded,
                        size: 18,
                        color: sound ? Palette.breath : Palette.mist),
                    const SizedBox(width: 8),
                    Text(sound ? '소리 정보 수신 중' : '마이크 연결 확인',
                        style: Typo.sans(18, color: Palette.mist))
                  ]))),
        ]));
  }
}

class _Reading extends StatelessWidget {
  const _Reading(
      {required this.icon,
      required this.label,
      required this.value,
      required this.note,
      required this.extra,
      this.unit,
      this.number = false,
      this.color = Palette.moon});
  final IconData icon;
  final String label, value, note;
  final String? unit;
  final bool number;
  final Color color;
  final Widget extra;
  @override
  Widget build(BuildContext context) => Container(
      padding: const EdgeInsets.all(24),
      decoration: BoxDecoration(
          color: Palette.inkRaised,
          borderRadius: BorderRadius.circular(22),
          border: Border.all(color: Palette.line)),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Row(children: [
          Icon(icon, size: 22, color: Palette.breath),
          const SizedBox(width: 10),
          Text(label, style: Typo.sans(20, weight: 540, color: Palette.moonDim))
        ]),
        const SizedBox(height: 8),
        Row(
            crossAxisAlignment: CrossAxisAlignment.baseline,
            textBaseline: TextBaseline.alphabetic,
            children: [
              Flexible(
                  child: Text(value,
                      maxLines: 1,
                      style: Typo.sans(number ? 52 : 34,
                          weight: 560,
                          color: color,
                          height: 1.2,
                          tabular: true))),
              if (unit != null) ...[
                const SizedBox(width: 8),
                Text(unit!, style: Typo.sans(20, color: Palette.mist))
              ]
            ]),
        const SizedBox(height: 6),
        Text(note, maxLines: 2, style: Typo.sans(17, color: Palette.moonDim)),
        const Spacer(),
        extra,
      ]));
}
