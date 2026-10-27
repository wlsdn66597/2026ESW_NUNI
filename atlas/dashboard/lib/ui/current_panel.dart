import 'package:flutter/material.dart';
import '../copy.dart';
import '../data/care_history.dart';
import '../data/store.dart';
import '../theme.dart';
import 'common.dart';

class CurrentPanel extends StatelessWidget {
  const CurrentPanel(
      {super.key,
      required this.store,
      required this.history,
      required this.acknowledged,
      required this.onAcknowledge,
      this.recordId,
      this.compact = false});
  final NuniStore store;
  final CareHistory history;
  final bool acknowledged;
  final VoidCallback onAcknowledge;
  final String? recordId;
  final bool compact;

  @override
  Widget build(BuildContext context) {
    final crying = store.soundAvailable && store.sensors.cry?.isCrying == true;
    final alert = store.link == Link.ok && store.state.level == 'alert';
    final known = store.soundAvailable;
    final current = store.cry?.current;
    final pending = store.cry?.episode(store.cry?.pending);
    final id = recordId ??
        (known && current != null
            ? current.id
            : pending?.action == null
                ? pending?.id
                : null);
    final choice = history.choices[id];
    final stateText = store.link == Link.lost
        ? '연결을 확인해 주세요'
        : alert
            ? '아기를 확인해 주세요'
            : crying
                ? '울음이 감지됐어요'
                : known
                    ? '지금은 울음이 없어요'
                    : '소리 정보를 기다려요';
    final asset = alert || !known
        ? 'nuni_penguin_black'
        : crying
            ? 'penguin_crying'
            : 'penguin_calm';
    final stateCard = Container(
      padding: EdgeInsets.all(compact ? 20 : 28),
      decoration: BoxDecoration(
          color: alert
              ? Palette.rose
              : crying
                  ? Palette.peach
                  : Palette.sage,
          borderRadius: BorderRadius.circular(16)),
      child:
          Flex(direction: compact ? Axis.vertical : Axis.horizontal, children: [
        SizedBox(
            width: compact ? 150 : 270,
            height: compact ? 120 : null,
            child: AnimatedSwitcher(
                duration: MediaQuery.of(context).disableAnimations
                    ? Duration.zero
                    : const Duration(milliseconds: 220),
                child: Image.asset('assets/images/$asset.png',
                    key: ValueKey('penguin-$asset'),
                    fit: BoxFit.contain,
                    semanticLabel: crying
                        ? '울음 감지 펭귄'
                        : known
                            ? '울음 미감지 펭귄'
                            : '상태 확인 펭귄'))),
        SizedBox(width: compact ? 0 : 24, height: compact ? 8 : 0),
        Expanded(
            child: Column(
                mainAxisAlignment: MainAxisAlignment.center,
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
              Text(stateText,
                  style: Typo.sans(compact ? 28 : 39,
                      weight: 680,
                      height: 1.3,
                      color: alert ? Palette.alarm : Palette.moon)),
              if (alert) ...[
                const SizedBox(height: 10),
                Text(store.state.reasons.map(reasonText).toSet().join(' · '),
                    style: Typo.sans(20, color: Palette.alarm)),
                if (!acknowledged) ...[
                  const SizedBox(height: 12),
                  NuniButton(
                      label: '확인했어요',
                      keyHint: 'Enter',
                      tone: Tone.alarm,
                      size: 21,
                      onPressed: onAcknowledge)
                ],
              ],
              if ((crying || id != null) && !alert) ...[
                SizedBox(height: compact ? 12 : 20),
                Text(crying ? '어떤 돌봄을 하고 있나요?' : '무엇을 했더니 그쳤나요?',
                    style: Typo.sans(compact ? 20 : 23, weight: 600)),
                const SizedBox(height: 12),
                Wrap(spacing: 8, runSpacing: 8, children: [
                  for (final action in const ['feed', 'diaper', 'hold'])
                    _ResponseButton(
                        action: action,
                        selected: choice == action,
                        enabled: id != null &&
                            store.link == Link.ok &&
                            !history.sending(id),
                        onTap: () => history.respond(id!, action)),
                ]),
                const SizedBox(height: 10),
                Text(
                    id == null
                        ? '울음 기록을 준비하고 있어요'
                        : choice != null
                            ? history.failed(id)
                                ? '저장하지 못했어요 · 다시 눌러 주세요'
                                : '${store.label(choice)} 선택 · 울음 종료 후 저장'
                            : '수유·기저귀·안아주기 중 골라 주세요',
                    style: Typo.sans(17,
                        color: history.failed(id ?? '')
                            ? Palette.alarm
                            : Palette.moonDim)),
              ],
            ])),
      ]),
    );
    if (compact) {
      return Row(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        Expanded(flex: 5, child: stateCard),
        const SizedBox(width: 18),
        Expanded(
            flex: 6,
            child: Column(children: [
              SizedBox(
                  height: 148,
                  child: Row(children: [
                    Expanded(
                        child: _Observation(
                            label: '호흡 수',
                            value: store.breathObserved
                                ? '${store.radar!.breathingRate!.round()}'
                                : '확인 중',
                            unit: store.breathObserved ? '회/분' : '',
                            note:
                                store.breathObserved ? '현재 호흡 신호' : '호흡 신호 대기',
                            icon: Icons.favorite_border_rounded)),
                    const SizedBox(width: 12),
                    Expanded(
                        child: _Observation(
                            label: '울음 여부',
                            value: !known
                                ? '확인 중'
                                : crying
                                    ? '울음 감지'
                                    : '울음 없음',
                            unit: '',
                            note: known ? '최근 소리 기준' : '소리 정보 대기',
                            icon: Icons.hearing_rounded)),
                  ])),
              const SizedBox(height: 12),
              _CurrentEnvironment(store: store),
              const Spacer(),
              SizedBox(
                  height: 64,
                  child: Row(children: [
                    NuniButton(
                        label: '수유했어요',
                        keyHint: 'F',
                        size: 18,
                        onPressed:
                            store.link == Link.ok ? store.logFeed : null),
                    const SizedBox(width: 12),
                    Expanded(
                        child: Text(
                            store.notice ??
                                (store.cry?.lastFeed == null
                                    ? '마지막 수유 기록을 남겨 주세요'
                                    : '마지막 수유 ${ago(store.cry!.lastFeed!, DateTime.now())}'),
                            maxLines: 2,
                            style: Typo.sans(16,
                                color: store.noticeIsError
                                    ? Palette.alarm
                                    : Palette.moonDim))),
                  ])),
            ])),
      ]);
    }
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      Row(children: [
        Text('현재 상태', style: Typo.sans(30, weight: 700)),
        const Spacer(),
        Icon(Icons.circle,
            size: 8,
            color: store.link == Link.ok ? Palette.breath : Palette.mist),
        const SizedBox(width: 8),
        Text(store.isDemo ? '화면 미리보기' : '실시간',
            style: Typo.sans(18, color: Palette.moonDim))
      ]),
      const SizedBox(height: 20),
      Expanded(child: stateCard),
      const SizedBox(height: 20),
      SizedBox(
          height: 148,
          child: Row(children: [
            Expanded(
                child: _Observation(
                    label: '호흡 수',
                    value: store.breathObserved
                        ? '${store.radar!.breathingRate!.round()}'
                        : '확인 중',
                    unit: store.breathObserved ? '회/분' : '',
                    note: store.breathObserved
                        ? '센서가 확인한 현재 호흡'
                        : '유효한 호흡 신호를 기다려요',
                    icon: Icons.favorite_border_rounded)),
            const SizedBox(width: 16),
            Expanded(
                child: _Observation(
                    label: '울음 여부',
                    value: !known
                        ? '확인 중'
                        : crying
                            ? '울음 감지'
                            : '울음 없음',
                    unit: '',
                    note: known ? '마이크로 받은 최근 소리 기준' : '마이크의 새 정보를 기다려요',
                    icon: Icons.hearing_rounded,
                    color: crying ? Palette.lamp : Palette.moon)),
          ])),
      const SizedBox(height: 20),
      _CurrentEnvironment(store: store),
      const SizedBox(height: 18),
      SizedBox(
          height: 64,
          child: Row(children: [
            NuniButton(
                label: '수유했어요',
                keyHint: 'F',
                size: 21,
                onPressed: store.link == Link.ok ? store.logFeed : null),
            const SizedBox(width: 18),
            Expanded(
                child: Text(
                    store.notice ??
                        (store.cry?.lastFeed == null
                            ? '마지막 수유 기록을 남겨 주세요'
                            : '마지막 수유 ${ago(store.cry!.lastFeed!, DateTime.now())}'),
                    maxLines: 2,
                    style: Typo.sans(18,
                        color: store.noticeIsError
                            ? Palette.alarm
                            : Palette.moonDim))),
          ])),
    ]);
  }
}

class _ResponseButton extends StatelessWidget {
  const _ResponseButton(
      {required this.action,
      required this.selected,
      required this.enabled,
      required this.onTap});
  final String action;
  final bool selected, enabled;
  final VoidCallback onTap;
  @override
  Widget build(BuildContext context) => OutlinedButton(
        key: ValueKey('response-$action'),
        onPressed: enabled ? onTap : null,
        style: OutlinedButton.styleFrom(
            backgroundColor: selected ? Palette.breath : Palette.inkRaised,
            foregroundColor: selected ? Colors.white : Palette.moon,
            side: BorderSide(color: selected ? Palette.breath : Palette.line),
            shape:
                RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
            padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 13)),
        child: Text({'feed': '수유', 'diaper': '기저귀', 'hold': '안아주기'}[action]!,
            style: Typo.sans(21,
                weight: 580, color: selected ? Colors.white : Palette.moon)),
      );
}

class _Observation extends StatelessWidget {
  const _Observation(
      {required this.label,
      required this.value,
      required this.unit,
      required this.note,
      required this.icon,
      this.color = Palette.moon});
  final String label, value, unit, note;
  final IconData icon;
  final Color color;
  @override
  Widget build(BuildContext context) => Container(
      padding: const EdgeInsets.all(20),
      width: double.infinity,
      decoration: BoxDecoration(
          color: Palette.inkRaised,
          borderRadius: BorderRadius.circular(16),
          border: Border.all(color: Palette.line)),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Row(children: [
          Icon(icon, size: 20, color: Palette.breath),
          const SizedBox(width: 8),
          Text(label, style: Typo.sans(19, color: Palette.moonDim))
        ]),
        const SizedBox(height: 6),
        Row(
            crossAxisAlignment: CrossAxisAlignment.baseline,
            textBaseline: TextBaseline.alphabetic,
            children: [
              Text(value,
                  style: Typo.sans(35,
                      weight: 680, color: color, tabular: true, height: 1.2)),
              const SizedBox(width: 8),
              Text(unit, style: Typo.sans(18, color: Palette.mist))
            ]),
        const SizedBox(height: 5),
        Text(note, style: Typo.sans(16, color: Palette.moonDim)),
      ]));
}

class _CurrentEnvironment extends StatelessWidget {
  const _CurrentEnvironment({required this.store});
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
    return Container(
        height: 198,
        padding: const EdgeInsets.all(22),
        decoration: BoxDecoration(
            color: Palette.inkRaised,
            borderRadius: BorderRadius.circular(16),
            border: Border.all(color: Palette.line)),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Text('아기방 환경', style: Typo.sans(23, weight: 650)),
          const SizedBox(height: 15),
          Row(children: [
            Expanded(child: _RoomValue('온도', room?.temp, '℃', 1)),
            Expanded(child: _RoomValue('습도', room?.humidity, '%', 0)),
            Expanded(child: _RoomValue('환기 · CO2', room?.co2, 'ppm', 0)),
          ]),
          const Spacer(),
          Text(
              room == null
                  ? '환경 센서의 새 정보를 기다려요'
                  : advice.isEmpty
                      ? '현재 온도·습도·CO2를 수신하고 있어요'
                      : advice.first,
              style: Typo.sans(18,
                  color: advice.isEmpty ? Palette.moonDim : Palette.lamp),
              maxLines: 1,
              overflow: TextOverflow.ellipsis),
        ]));
  }
}

class _RoomValue extends StatelessWidget {
  const _RoomValue(this.label, this.value, this.unit, this.places);
  final String label, unit;
  final double? value;
  final int places;
  @override
  Widget build(BuildContext context) =>
      Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Text(label, style: Typo.sans(17, color: Palette.mist)),
        Row(
            crossAxisAlignment: CrossAxisAlignment.baseline,
            textBaseline: TextBaseline.alphabetic,
            children: [
              Text(
                  value != null && value!.isFinite
                      ? value!.toStringAsFixed(places)
                      : '–',
                  style: Typo.sans(31, weight: 630, tabular: true)),
              const SizedBox(width: 5),
              Text(unit, style: Typo.sans(17, color: Palette.moonDim))
            ]),
      ]);
}
