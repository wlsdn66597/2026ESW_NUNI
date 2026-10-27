import 'dart:math' as math;
import 'package:flutter/material.dart';
import '../copy.dart';
import '../data/care_history.dart';
import '../data/store.dart';
import '../theme.dart';
import 'radio_info.dart';

class HistoryPanel extends StatelessWidget {
  const HistoryPanel(
      {super.key,
      required this.view,
      required this.store,
      required this.onSample,
      required this.onRecord});
  final HistoryView view;
  final NuniStore store;
  final ValueChanged<bool> onSample;
  final ValueChanged<CareRecord> onRecord;
  @override
  Widget build(BuildContext context) =>
      Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Row(children: [
          Text('과거 기록', style: Typo.sans(30, weight: 700)),
          const SizedBox(width: 16),
          Text(view.sample ? '지난 7일 · 합성 예시' : '최근 7일 · 실제 기록',
              key: const ValueKey('history-source'),
              style: Typo.sans(17,
                  color: view.sample ? Palette.lamp : Palette.moonDim)),
          const Spacer(),
          SegmentedButton<bool>(
              key: const ValueKey('history-switch'),
              segments: const [
                ButtonSegment(value: true, label: Text('예시')),
                ButtonSegment(value: false, label: Text('실제'))
              ],
              selected: {view.sample},
              showSelectedIcon: false,
              style: SegmentedButton.styleFrom(
                  textStyle: Typo.sans(17, weight: 540),
                  selectedBackgroundColor: Palette.sage,
                  selectedForegroundColor: Palette.breath,
                  foregroundColor: Palette.moonDim,
                  padding: const EdgeInsets.symmetric(horizontal: 12)),
              onSelectionChanged: (values) => onSample(values.first)),
        ]),
        const SizedBox(height: 20),
        _Surface(
            height: 216,
            child:
                Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              Row(children: [
                Text('언제 자주 울었을까요?', style: Typo.sans(23, weight: 650)),
                const Spacer(),
                Text('${view.records.length}회',
                    style: Typo.sans(20, color: Palette.moonDim, tabular: true))
              ]),
              const SizedBox(height: 12),
              SizedBox(
                  height: 83,
                  child: Semantics(
                      label: '시간대별 울음 횟수',
                      child: CustomPaint(
                          size: const Size(double.infinity, 83),
                          painter: _HourlyChart(view.hourly, view.peakBlock)))),
              const SizedBox(height: 9),
              Text(
                  view.records.isEmpty
                      ? '울음 기록이 쌓이면 시간대를 보여드려요'
                      : '${HistoryView.blockLabels[view.peakBlock]}에 가장 자주 울었어요',
                  style: Typo.sans(21, weight: 560, color: Palette.breath)),
            ])),
        const SizedBox(height: 16),
        _Surface(
            height: 220,
            child:
                Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              Text('울음이 그쳤던 기록', style: Typo.sans(23, weight: 650)),
              const SizedBox(height: 8),
              if (view.records.isEmpty)
                Padding(
                    padding: const EdgeInsets.only(top: 18),
                    child: Text('수유·기저귀·안아주기 기록을 이곳에 모아요',
                        style: Typo.sans(20, color: Palette.moonDim)))
              else
                for (final r in view.records.take(3))
                  SizedBox(
                      height: 44,
                      child: Row(children: [
                        SizedBox(
                            width: 166,
                            child: Text(_date(r.at),
                                style: Typo.sans(18,
                                    color: Palette.moonDim, tabular: true))),
                        Icon(
                            r.action == 'feed'
                                ? Icons.water_drop_outlined
                                : r.action == 'diaper'
                                    ? Icons.child_care_rounded
                                    : Icons.volunteer_activism_outlined,
                            size: 21,
                            color: Palette.breath),
                        const SizedBox(width: 10),
                        Expanded(
                            child: Text(
                                r.action == null
                                    ? '기록 대기'
                                    : store.label(r.action!),
                                maxLines: 1,
                                overflow: TextOverflow.ellipsis,
                                style: Typo.sans(21, weight: 560))),
                        Text(
                            '${span(Duration(seconds: r.seconds), seconds: false)} 울음',
                            style: Typo.sans(16, color: Palette.mist)),
                        if (!view.sample && r.action == null) ...[
                          const SizedBox(width: 8),
                          TextButton(
                              onPressed: () => onRecord(r),
                              child: const Text('기록'))
                        ],
                      ])),
            ])),
        const SizedBox(height: 16),
        _Surface(
            height: 210,
            color: Palette.sage,
            child:
                Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              Text('울음이 없었던 환경', style: Typo.sans(23, weight: 650)),
              const SizedBox(height: 8),
              Text(
                  view.temperatureRange == null
                      ? '환경 기록을 더 모으고 있어요'
                      : '${view.temperatureRange}  ·  습도 ${view.humidityRange}',
                  style: Typo.sans(29, weight: 650, color: Palette.breath)),
              const SizedBox(height: 6),
              Text('10분 이상 울음 미감지 구간 · ${view.quiet.length}개 환경 기록',
                  style: Typo.sans(16, color: Palette.moonDim)),
              const Spacer(),
              const Divider(height: 16, color: Palette.line),
              Text(
                  view.temperatureRange == null
                      ? '기록이 모이면 환경 조절 범위를 제안해요'
                      : '기록 기반 제안 · 이 범위부터 참고해 보세요',
                  style: Typo.sans(20, weight: 540, color: Palette.breath)),
            ])),
        const Spacer(),
        const SizedBox(height: 132, child: RadioInfoCard()),
      ]);
  String _date(DateTime t) {
    final now = DateTime.now();
    final days = DateTime(now.year, now.month, now.day)
        .difference(DateTime(t.year, t.month, t.day))
        .inDays;
    return '${days == 0 ? '오늘' : days == 1 ? '어제' : '${t.month}/${t.day}'} ${clock(t)}';
  }
}

class _Surface extends StatelessWidget {
  const _Surface(
      {required this.height,
      required this.child,
      this.color = Palette.inkRaised});
  final double height;
  final Widget child;
  final Color color;
  @override
  Widget build(BuildContext context) => Container(
      width: double.infinity,
      height: height,
      padding: const EdgeInsets.all(20),
      decoration: BoxDecoration(
          color: color,
          borderRadius: BorderRadius.circular(16),
          border: color == Palette.inkRaised
              ? Border.all(color: Palette.line)
              : null),
      child: child);
}

class _HourlyChart extends CustomPainter {
  _HourlyChart(this.counts, this.peak);
  final List<int> counts;
  final int peak;
  @override
  void paint(Canvas canvas, Size size) {
    const chartHeight = 54.0;
    final maxCount = math.max(1, counts.fold<int>(0, math.max));
    final step = size.width / 24;
    for (var i = 0; i < 24; i++) {
      final h = counts[i] == 0 ? 2.0 : counts[i] / maxCount * chartHeight;
      canvas.drawRRect(
          RRect.fromRectAndRadius(
              Rect.fromLTWH(i * step + 3, chartHeight - h, step - 6, h),
              const Radius.circular(3)),
          Paint()
            ..color = counts[i] == 0
                ? Palette.line
                : i ~/ 3 == peak
                    ? Palette.breath
                    : const Color(0xFF86B29D));
    }
    for (final hour in [0, 6, 12, 18, 24]) {
      final painter = TextPainter(
          text: TextSpan(
              text: '$hour시', style: Typo.sans(14, color: Palette.mist)),
          textDirection: TextDirection.ltr)
        ..layout();
      painter.paint(
          canvas,
          Offset(
              (hour / 24 * size.width - painter.width / 2)
                  .clamp(0.0, size.width - painter.width),
              61));
    }
  }

  @override
  bool shouldRepaint(covariant _HourlyChart old) =>
      old.peak != peak || !_same(old.counts, counts);
  bool _same(List<int> a, List<int> b) {
    for (var i = 0; i < a.length; i++) {
      if (a[i] != b[i]) return false;
    }
    return true;
  }
}
