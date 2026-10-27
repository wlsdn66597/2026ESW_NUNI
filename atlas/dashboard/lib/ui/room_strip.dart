import 'package:flutter/material.dart';
import '../copy.dart';
import '../data/store.dart';
import '../theme.dart';

/// 현재 연결된 온습도·CO2 센서만 표시한다. 실제 가전 동작과 권고를 혼동하지 않는다.
class RoomStrip extends StatelessWidget {
  const RoomStrip({super.key, required this.store});
  final NuniStore store;
  @override
  Widget build(BuildContext context) {
    final room = store.roomAvailable ? store.sensors.room : null;
    final advice = room == null
        ? <String>[]
        : store.state.actions
            .map((a) => roomAdvice[a])
            .whereType<String>()
            .toList();
    String value(double? v, int places) =>
        v == null || !v.isFinite ? '–' : v.toStringAsFixed(places);
    return SizedBox(
        height: 72,
        child: Row(children: [
          SizedBox(
              width: 138,
              child: Text('아기방 환경',
                  style: Typo.sans(22, weight: 540, color: Palette.moonDim))),
          _Value(
              icon: Icons.thermostat_rounded,
              label: '실내 온도',
              value: value(room?.temp, 1),
              unit: '℃'),
          _Value(
              icon: Icons.water_drop_outlined,
              label: '습도',
              value: value(room?.humidity, 0),
              unit: '%'),
          _Value(
              icon: Icons.air_rounded,
              label: '환기 지표 · CO2',
              value: value(room?.co2, 0),
              unit: 'ppm'),
          const SizedBox(width: 18),
          Expanded(
              child: Column(
                  mainAxisAlignment: MainAxisAlignment.center,
                  crossAxisAlignment: CrossAxisAlignment.end,
                  children: [
                Text(
                    room == null
                        ? '환경 센서의 새 정보를 기다려요'
                        : advice.isEmpty
                            ? '온습도와 환기 상태를 살펴보고 있어요'
                            : advice.first,
                    maxLines: 2,
                    textAlign: TextAlign.right,
                    style: Typo.sans(22,
                        weight: advice.isEmpty ? 420 : 520,
                        color:
                            advice.isEmpty ? Palette.moonDim : Palette.lamp)),
                if (advice.isNotEmpty) ...[
                  const SizedBox(height: 3),
                  Text('돌봄을 위한 권고예요 · 가전 작동 상태가 아니에요',
                      style: Typo.sans(16, color: Palette.mist))
                ],
              ])),
        ]));
  }
}

class _Value extends StatelessWidget {
  const _Value(
      {required this.icon,
      required this.label,
      required this.value,
      required this.unit});
  final IconData icon;
  final String label, value, unit;
  @override
  Widget build(BuildContext context) => Padding(
      padding: const EdgeInsets.only(right: 34),
      child: Row(children: [
        Icon(icon, size: 25, color: Palette.mist),
        const SizedBox(width: 10),
        Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              Text(label, style: Typo.sans(17, color: Palette.mist)),
              Row(
                  crossAxisAlignment: CrossAxisAlignment.baseline,
                  textBaseline: TextBaseline.alphabetic,
                  children: [
                    Text(value,
                        style: Typo.sans(30, weight: 540, tabular: true)),
                    const SizedBox(width: 5),
                    Text(unit, style: Typo.sans(18, color: Palette.mist))
                  ]),
            ])
      ]));
}
