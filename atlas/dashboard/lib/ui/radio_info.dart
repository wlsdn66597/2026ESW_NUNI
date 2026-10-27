import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import '../theme.dart';

const radioSpecUrl =
    'https://files.seeedstudio.com/wiki/mmwave-for-xiao/mr60/datasheet/MR60BHA2_Breathing_and_Heartbeat_Module.pdf';
const radioStandardUrl =
    'https://www.rra.go.kr/emf/wrongfact/knowing/index08.do';

/// 문서 근거를 보여준다. 센서 거리를 전파 노출 측정으로 표시하지 않는다.
class RadioInfoCard extends StatelessWidget {
  const RadioInfoCard({super.key});
  @override
  Widget build(BuildContext context) => Container(
        padding: const EdgeInsets.symmetric(horizontal: 22, vertical: 16),
        decoration: BoxDecoration(
            color: Palette.inkRaised,
            borderRadius: BorderRadius.circular(24),
            border: Border.all(color: Palette.line)),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Row(children: [
            const Icon(Icons.info_outline_rounded,
                size: 20, color: Palette.moonDim),
            const SizedBox(width: 10),
            Text('레이더 전파 정보',
                style: Typo.sans(19, weight: 600, color: Palette.moonDim)),
            const Spacer(),
            TextButton(
              key: const ValueKey('radio-info-button'),
              onPressed: () => showRadioInfo(context),
              style: TextButton.styleFrom(
                padding: const EdgeInsets.symmetric(horizontal: 8),
                minimumSize: const Size(44, 30),
                tapTargetSize: MaterialTapTargetSize.shrinkWrap,
              ),
              child: Text('근거 보기', style: Typo.sans(17, color: Palette.breath)),
            ),
          ]),
          const SizedBox(height: 4),
          Text('60GHz · 송신 전력 약 16mW (12dBm)',
              style: Typo.sans(19, weight: 540, height: 1.4)),
          const SizedBox(height: 6),
          Text('제조사 기술사양서 기준 · Seeed MR60BHA2 4쪽',
              style: Typo.sans(16, color: Palette.moonDim, height: 1.4)),
        ]),
      );
}

Future<void> showRadioInfo(BuildContext context) => showDialog<void>(
      context: context,
      builder: (context) => Dialog(
        backgroundColor: Palette.inkRaised,
        shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(28),
            side: const BorderSide(color: Palette.line)),
        child: ConstrainedBox(
          constraints: BoxConstraints(
              maxWidth: 900,
              maxHeight: MediaQuery.sizeOf(context).height * .88),
          child: Padding(
              padding: const EdgeInsets.all(36),
              child: Column(
                  mainAxisSize: MainAxisSize.min,
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Row(children: [
                      const Icon(Icons.sensors_rounded,
                          color: Palette.breath, size: 30),
                      const SizedBox(width: 12),
                      Expanded(
                          child: Text('전파 안내와 검증 상태',
                              style: Typo.sans(30, weight: 600))),
                      IconButton(
                          tooltip: '닫기',
                          onPressed: () => Navigator.pop(context),
                          icon: const Icon(Icons.close_rounded))
                    ]),
                    const SizedBox(height: 18),
                    Flexible(
                        child: SingleChildScrollView(
                            child: Column(
                                crossAxisAlignment: CrossAxisAlignment.start,
                                children: [
                          Text('확인한 내용부터 투명하게 알려드려요.',
                              style: Typo.sans(23, color: Palette.moonDim)),
                          const SizedBox(height: 22),
                          const _Fact(
                              title: '사용 센서',
                              value: 'Seeed Studio MR60BHA2 · 비접촉 60GHz 레이더'),
                          const _Fact(
                              title: '제조사 RF 사양',
                              value: '송신 출력 12dBm (약 16mW) · 안테나 이득 4dBi'),
                          Text(
                              '제조사 기술사양서에 기재된 값이에요. 현재 송신량이나 아기 위치의 전파 노출을 실시간으로 측정한 값은 아니에요.',
                              style: Typo.sans(19, color: Palette.mist)),
                          const SizedBox(height: 22),
                          Container(
                              padding: const EdgeInsets.all(22),
                              decoration: BoxDecoration(
                                  color: Palette.ink,
                                  borderRadius: BorderRadius.circular(18),
                                  border: Border.all(color: Palette.line)),
                              child: Column(
                                  crossAxisAlignment: CrossAxisAlignment.start,
                                  children: [
                                    Text('완제품 전파 노출 시험: 아직 실시하지 않았어요',
                                        style: Typo.sans(22,
                                            weight: 560, color: Palette.lamp)),
                                    const SizedBox(height: 10),
                                    Text(
                                        '안전 인증이나 기준 대비 노출 비율을 확인한 단계는 아니에요. 센서 거리와 연결 상태만으로 전파 안전성을 판정하지 않아요.',
                                        style: Typo.sans(20,
                                            color: Palette.moonDim)),
                                  ])),
                          const SizedBox(height: 22),
                          Text('아래 자료를 참고했어요',
                              style: Typo.sans(22, weight: 560)),
                          const SizedBox(height: 10),
                          const _Source(
                              title: '제조사 MR60BHA2 기술사양서',
                              detail: '2024.03.05 Beta · RF 사양 4쪽',
                              url: radioSpecUrl),
                          const SizedBox(height: 12),
                          const _Source(
                              title: '국립전파연구원 전자파 인체보호기준 안내',
                              detail: '전파 노출을 평가하는 기준에 대한 공공기관 설명',
                              url: radioStandardUrl),
                          const SizedBox(height: 18),
                          Text(
                              'NUNI는 돌봄을 돕는 시제품이에요. 의료 진단이나 보호자의 직접 확인을 대신하지 않아요.',
                              style: Typo.sans(18, color: Palette.mist)),
                        ]))),
                  ])),
        ),
      ),
    );

class _Fact extends StatelessWidget {
  const _Fact({required this.title, required this.value});
  final String title, value;
  @override
  Widget build(BuildContext context) => Padding(
      padding: const EdgeInsets.only(bottom: 14),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Text(title, style: Typo.sans(18, color: Palette.mist)),
        const SizedBox(height: 4),
        Text(value, style: Typo.sans(22, weight: 520))
      ]));
}

class _Source extends StatelessWidget {
  const _Source({required this.title, required this.detail, required this.url});
  final String title, detail, url;
  @override
  Widget build(BuildContext context) =>
      Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Expanded(
            child:
                Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Text(title, style: Typo.sans(20)),
          const SizedBox(height: 3),
          Text(detail, style: Typo.sans(17, color: Palette.mist))
        ])),
        TextButton(
            onPressed: () async {
              await Clipboard.setData(ClipboardData(text: url));
              if (context.mounted)
                ScaffoldMessenger.of(context).showSnackBar(
                    const SnackBar(content: Text('자료 주소를 복사했어요.')));
            },
            child: Text('주소 복사', style: Typo.sans(17, color: Palette.breath))),
      ]);
}
