import 'package:flutter/material.dart';

import '../copy.dart';
import '../data/models.dart';
import '../data/store.dart';
import '../theme.dart';
import 'common.dart';

/// 울음 도우미 — 이 화면에서 유일하게 손이 가는 판.
///   울음 중    먼저 해 볼 1·2순위(답이 k_min번 모인 뒤) + '해 봤는데 안 그쳤어요'
///   울음 뒤    "무엇으로 그쳤나요?" — 답 하나가 이 아기 기록에 쌓인다(키 1~7)
///   평소       답이 얼마나 모였는지, 최근 울음, 수유 기록
class CryHelper extends StatelessWidget {
  const CryHelper({
    super.key,
    required this.store,
    required this.askingId,
    required this.onAsk,
    required this.onLater,
  });

  final NuniStore store;
  final String? askingId; // 지금 묻고 있는 울음(없으면 평소 화면)
  final ValueChanged<String> onAsk;
  final VoidCallback onLater;

  @override
  Widget build(BuildContext context) {
    final log = store.cry;
    final current = log?.current;
    final asking = log?.episode(askingId);
    final Widget body;
    if (log == null) {
      body = _Waiting(linkLost: store.link == Link.lost);
    } else if (current != null && !store.soundAvailable) {
      body = const _Waiting(linkLost: true);
    } else if (current != null) {
      body = _Crying(store: store, log: log, now: current);
    } else if (asking != null) {
      body = _Question(store: store, log: log, ep: asking, onLater: onLater);
    } else {
      body = _Idle(store: store, log: log, onAsk: onAsk);
    }
    return Container(
      padding: const EdgeInsets.fromLTRB(40, 34, 40, 30),
      decoration: BoxDecoration(
        color: Palette.inkRaised,
        borderRadius: BorderRadius.circular(32),
        border: Border.all(color: Palette.line),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(children: [
            Container(
              width: 36,
              height: 36,
              decoration: BoxDecoration(
                  color: Palette.peach,
                  borderRadius: BorderRadius.circular(12)),
              child: const Icon(Icons.volunteer_activism_outlined,
                  size: 21, color: Palette.lamp),
            ),
            const SizedBox(width: 10),
            Text('돌봄 도우미', style: Typo.sans(22, weight: 600)),
          ]),
          const SizedBox(height: 18),
          if (store.state.level == 'alert') ...[
            Container(
              width: double.infinity,
              padding: const EdgeInsets.all(14),
              decoration: BoxDecoration(
                  color: Palette.rose, borderRadius: BorderRadius.circular(14)),
              child: Text(
                store.state.reasons.isEmpty
                    ? '확인이 필요한 신호가 감지됐어요'
                    : store.state.reasons.map(reasonText).toSet().join(' · '),
                style: Typo.sans(20, weight: 560, color: Palette.alarm),
              ),
            ),
            const SizedBox(height: 14),
          ],
          Expanded(
            child: LayoutBuilder(builder: (context, box) {
              return SingleChildScrollView(
                child: ConstrainedBox(
                  constraints: BoxConstraints(minHeight: box.maxHeight),
                  child: IntrinsicHeight(child: body),
                ),
              );
            }),
          ),
          _Notice(store: store),
        ],
      ),
    );
  }
}

class _Waiting extends StatelessWidget {
  const _Waiting({required this.linkLost});
  final bool linkLost;

  @override
  Widget build(BuildContext context) => Text(
        linkLost ? '센서 서비스와 연결되면 울음 기록을 불러와요.' : '울음 기록을 불러오고 있어요.',
        style: Typo.sans(24, color: Palette.moonDim),
      );
}

/// 울음 중
class _Crying extends StatelessWidget {
  const _Crying({required this.store, required this.log, required this.now});
  final NuniStore store;
  final CryLogView log;
  final CryNow now;

  @override
  Widget build(BuildContext context) {
    final hint = now.hint;
    final showHint = hint != null && hint.show2 && hint.top2.length >= 2;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        if (showHint) ...[
          _Try(
              store: store,
              rank: '먼저 해 볼 것',
              action: hint.top2[0],
              p: hint.probs[hint.top2[0]],
              tried: hint.notTried.contains(hint.top2[0]),
              big: true,
              cryId: now.id),
          const SizedBox(height: 30),
          _Try(
              store: store,
              rank: '그다음',
              action: hint.top2[1],
              p: hint.probs[hint.top2[1]],
              tried: hint.notTried.contains(hint.top2[1]),
              big: false,
              cryId: now.id),
          const SizedBox(height: 24),
          Text('돌봄 기록과 최근 수유 시각을 바탕으로 한 참고용 제안이에요.',
              style: Typo.sans(19, color: Palette.mist)),
        ] else ...[
          Text('우리 아기에게 맞춰가요', style: Typo.sans(36, weight: 650)),
          const SizedBox(height: 14),
          Text(
              log.nLabeled == 0
                  ? '돌봄 기록이 ${log.kMin}번 쌓이면 울 때 먼저 살펴볼 것을 함께 알려드려요.'
                  : '돌봄 기록이 ${log.nLabeled}번 쌓였어요. ${log.kMin}번부터 이전 기록을 함께 참고해요.',
              style: Typo.sans(22, color: Palette.moonDim, height: 1.5)),
          const SizedBox(height: 22),
          _Progress(log: log),
        ],
        const Spacer(),
        _FeedRow(store: store, log: log),
      ],
    );
  }
}

class _Try extends StatelessWidget {
  const _Try(
      {required this.store,
      required this.rank,
      required this.action,
      required this.p,
      required this.tried,
      required this.big,
      required this.cryId});
  final NuniStore store;
  final String rank, action, cryId;
  final double? p;
  final bool tried, big;

  @override
  Widget build(BuildContext context) => Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(rank, style: Typo.sans(20, weight: 520, color: Palette.mist)),
          const SizedBox(height: 2),
          Text(
            store.label(action),
            style: Typo.sans(big ? 60 : 40,
                    weight: 560, color: tried ? Palette.mist : Palette.moon)
                .copyWith(
              decoration: tried ? TextDecoration.lineThrough : null,
              decorationColor: Palette.mist,
              decorationThickness: 2,
            ),
          ),
          const SizedBox(height: 10),
          if (tried)
            Text('해 봤는데 안 그쳤다고 기록했어요',
                style: Typo.sans(20, color: Palette.mist))
          else
            NuniButton(
                label: '해 봤는데 안 그쳤어요',
                size: 20,
                onPressed: () => store.answerNot(cryId, action)),
        ],
      );
}

/// 울음이 끝난 뒤 묻기
class _Question extends StatelessWidget {
  const _Question(
      {required this.store,
      required this.log,
      required this.ep,
      required this.onLater});
  final NuniStore store;
  final CryLogView log;
  final CryEpisode ep;
  final VoidCallback onLater;

  @override
  Widget build(BuildContext context) {
    final primary = log.answers.where((a) => log.actions.contains(a)).toList();
    final secondary =
        log.answers.where((a) => !log.actions.contains(a)).toList();
    var key = 0;
    Widget btn(String a, double size) {
      key++;
      return NuniButton(
        label: answerButtonText[a] ?? store.label(a),
        keyHint: '$key',
        size: size,
        expand: true,
        onPressed: () => store.answer(ep.id, a),
      );
    }

    final tried = ep.notTried.map(store.label).toList();
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text('어떻게 진정됐나요?', style: Typo.sans(40, weight: 650)),
        const SizedBox(height: 10),
        Text('${clock(ep.t0)}에 시작해 ${span(ep.length)} 동안 울었어요.',
            style: Typo.sans(22, color: Palette.moonDim)),
        if (tried.isNotEmpty)
          Padding(
            padding: const EdgeInsets.only(top: 4),
            child: Text('해 봤는데 안 그친 것: ${tried.join(', ')}',
                style: Typo.sans(19, color: Palette.mist)),
          ),
        const SizedBox(height: 22),
        for (final a in primary) ...[btn(a, 25), const SizedBox(height: 10)],
        const SizedBox(height: 8),
        Wrap(
          spacing: 10,
          runSpacing: 10,
          children: [
            for (final a in secondary) SizedBox(width: 240, child: btn(a, 20))
          ],
        ),
        const Spacer(),
        NuniButton(
            label: '나중에 답할게요',
            keyHint: 'Esc',
            tone: Tone.quiet,
            size: 21,
            onPressed: onLater),
      ],
    );
  }
}

/// 평소
class _Idle extends StatelessWidget {
  const _Idle({required this.store, required this.log, required this.onAsk});
  final NuniStore store;
  final CryLogView log;
  final ValueChanged<String> onAsk;

  @override
  Widget build(BuildContext context) {
    final recent = log.episodes.reversed.take(4).toList();
    final ready = log.nLabeled >= log.kMin;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text('작은 기록이 쌓여요', style: Typo.sans(36, weight: 650)),
        const SizedBox(height: 14),
        if (!ready) ...[
          Text('돌봄 기록이 ${log.kMin}번 쌓이면 울 때 먼저 살펴볼 것을 함께 알려드려요.',
              style: Typo.sans(22, color: Palette.moonDim, height: 1.5)),
          const SizedBox(height: 18),
          _Progress(log: log),
        ] else
          Text('기록이 차곡차곡 쌓였어요. 다음 울음부터 최근 수유 시각과 돌봄 기록을 함께 살펴드려요.',
              style: Typo.sans(22, color: Palette.moonDim, height: 1.5)),
        const SizedBox(height: 30),
        Text('최근 울음', style: Typo.sans(20, weight: 520, color: Palette.mist)),
        const SizedBox(height: 8),
        if (recent.isEmpty)
          Text('첫 울음 기록을 기다리고 있어요.',
              style: Typo.sans(22, color: Palette.moonDim))
        else
          for (final e in recent)
            _EpisodeRow(store: store, ep: e, onAsk: onAsk),
        const Spacer(),
        _FeedRow(store: store, log: log),
      ],
    );
  }
}

class _EpisodeRow extends StatelessWidget {
  const _EpisodeRow(
      {required this.store, required this.ep, required this.onAsk});
  final NuniStore store;
  final CryEpisode ep;
  final ValueChanged<String> onAsk;

  @override
  Widget build(BuildContext context) => SizedBox(
        height: 46,
        child: Row(
          children: [
            SizedBox(
                width: 92,
                child: Text(clockShort(ep.t0),
                    style:
                        Typo.sans(22, tabular: true, color: Palette.moonDim))),
            SizedBox(
                width: 110,
                child: Text(span(ep.length, seconds: false),
                    style: Typo.sans(22, color: Palette.mist))),
            Expanded(
              child: ep.action != null
                  ? Text(store.label(ep.action!),
                      style: Typo.sans(22, weight: 500),
                      overflow: TextOverflow.ellipsis)
                  : Row(children: [
                      Text('기록 대기', style: Typo.sans(22, color: Palette.lamp)),
                      const SizedBox(width: 8),
                      NuniButton(
                          label: '기록하기',
                          tone: Tone.quiet,
                          size: 20,
                          onPressed: () => onAsk(ep.id)),
                    ]),
            ),
          ],
        ),
      );
}

class _Progress extends StatelessWidget {
  const _Progress({required this.log});
  final CryLogView log;

  @override
  Widget build(BuildContext context) => Row(
        children: [
          AnswerDots(done: log.nLabeled.clamp(0, log.kMin), total: log.kMin),
          const SizedBox(width: 8),
          Text('${log.nLabeled.clamp(0, log.kMin)} / ${log.kMin}번',
              style: Typo.sans(20, color: Palette.mist, tabular: true)),
        ],
      );
}

/// 수유 기록 — 맥락(마지막 수유 후 시간)에 쓰인다
class _FeedRow extends StatelessWidget {
  const _FeedRow({required this.store, required this.log});
  final NuniStore store;
  final CryLogView log;

  @override
  Widget build(BuildContext context) => Row(
        children: [
          NuniButton(
              label: '수유했어요', keyHint: 'F', size: 22, onPressed: store.logFeed),
          const SizedBox(width: 18),
          Expanded(
            child: Text(
              log.lastFeed == null
                  ? '수유 기록이 아직 없어요'
                  : '마지막 수유 ${ago(log.lastFeed!, DateTime.now())}',
              style: Typo.sans(20, color: Palette.mist),
              overflow: TextOverflow.ellipsis,
            ),
          ),
        ],
      );
}

class _Notice extends StatelessWidget {
  const _Notice({required this.store});
  final NuniStore store;

  @override
  Widget build(BuildContext context) => store.notice == null
      ? const SizedBox.shrink()
      : AnimatedOpacity(
          opacity: store.notice == null ? 0 : 1,
          duration: const Duration(milliseconds: 200),
          child: Padding(
            padding: const EdgeInsets.only(top: 14),
            child: Text(
              store.notice ?? ' ',
              style: Typo.sans(21,
                  weight: 540,
                  color: store.noticeIsError ? Palette.alarm : Palette.breath),
            ),
          ),
        );
}
