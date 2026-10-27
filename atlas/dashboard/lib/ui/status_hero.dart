import 'package:flutter/material.dart';

import '../data/store.dart';
import '../theme.dart';
import 'common.dart';

enum Mood { calm, watch, alarm, muted }

class HeroCopy {
  const HeroCopy(this.sentence, this.tone);
  final String sentence;
  final Mood tone;
}

/// 관측한 상태만 표현하며 무음·수면·자세나 안전을 추정하지 않는다.
HeroCopy describe(NuniStore s) {
  if (s.link == Link.lost) {
    return const HeroCopy('연결을 확인해 주세요', Mood.muted);
  }
  if (s.link == Link.connecting) {
    return const HeroCopy('연결하고 있어요', Mood.muted);
  }
  if (s.state.level == 'alert') {
    return const HeroCopy('아기를 확인해 주세요', Mood.alarm);
  }
  if (s.soundAvailable &&
      (s.cry?.current != null || s.sensors.cry?.isCrying == true)) {
    return const HeroCopy('울음이 감지됐어요', Mood.watch);
  }
  if (s.soundAvailable) {
    return const HeroCopy('지금은 울음이 없어요', Mood.calm);
  }
  if (s.breathObserved) {
    return const HeroCopy('호흡을 확인하고 있어요', Mood.calm);
  }
  return const HeroCopy('센서 연결을 기다려요', Mood.muted);
}

Color toneColor(Mood t) => switch (t) {
      Mood.calm => Palette.moon,
      Mood.watch => Palette.lamp,
      Mood.alarm => Palette.alarm,
      Mood.muted => Palette.moonDim,
    };

class StatusHero extends StatelessWidget {
  const StatusHero({
    super.key,
    required this.store,
    required this.reduceMotion,
    this.onAcknowledge,
    this.acknowledged = false,
  });
  final NuniStore store;
  final bool reduceMotion;
  final VoidCallback? onAcknowledge;
  final bool acknowledged;

  @override
  Widget build(BuildContext context) {
    final c = describe(store);
    final background = switch (c.tone) {
      Mood.alarm => Palette.rose,
      Mood.watch => Palette.peach,
      _ => Palette.sage,
    };
    return Container(
      key: const ValueKey('status-hero'),
      clipBehavior: Clip.antiAlias,
      padding: const EdgeInsets.fromLTRB(36, 28, 20, 24),
      decoration: BoxDecoration(
        color: background,
        borderRadius: BorderRadius.circular(32),
      ),
      child: Row(children: [
        Expanded(
          flex: 6,
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(children: [
                Icon(
                  c.tone == Mood.alarm
                      ? Icons.priority_high_rounded
                      : Icons.favorite_rounded,
                  size: 20,
                  color: toneColor(c.tone),
                ),
                const SizedBox(width: 9),
                Text('지금 아기 상태',
                    style: Typo.sans(21, weight: 540, color: Palette.moonDim)),
              ]),
              const Spacer(),
              AnimatedSwitcher(
                duration: Duration(milliseconds: reduceMotion ? 0 : 300),
                child: Text(
                  c.sentence,
                  key: ValueKey(c.sentence),
                  style: Typo.sans(54,
                      weight: 650, color: toneColor(c.tone), height: 1.3),
                  maxLines: 2,
                ),
              ),
              const Spacer(),
              if (c.tone == Mood.alarm &&
                  !acknowledged &&
                  onAcknowledge != null)
                NuniButton(
                  label: '확인했어요',
                  keyHint: 'Enter',
                  tone: Tone.alarm,
                  onPressed: onAcknowledge,
                  size: 23,
                ),
            ],
          ),
        ),
        const SizedBox(width: 12),
        Expanded(
          flex: 4,
          child: ExcludeSemantics(
            child: Image.asset(
              'assets/images/nuni_penguin_black.png',
              key: const ValueKey('nuni-penguin'),
              fit: BoxFit.contain,
              filterQuality: FilterQuality.medium,
            ),
          ),
        ),
      ]),
    );
  }
}
