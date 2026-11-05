import 'dart:async';

import 'package:flutter/material.dart';

import '../data/store.dart';
import '../theme.dart';
import 'common.dart';

/// 발표용 '가전 시연' 버튼(마우스로 누른다).
/// 30초 동안 시연용 온도·CO₂를 가전 정책에만 보내 에어컨·공기청정기 모형을 켠다.
/// 화면의 방 상태 숫자와 기록 DB는 실제 센서 값을 그대로 쓴다.
class ApplianceDemoButton extends StatefulWidget {
  const ApplianceDemoButton({super.key, required this.store});
  final NuniStore store;
  @override
  State<ApplianceDemoButton> createState() => _ApplianceDemoButtonState();
}

class _ApplianceDemoButtonState extends State<ApplianceDemoButton> {
  Timer? _tick;

  @override
  void dispose() {
    _tick?.cancel();
    super.dispose();
  }

  // 시연 중에만 1초마다 남은 시간을 새로 그린다(끝나면 멈춘다)
  void _syncTimer(bool active) {
    if (active && _tick == null) {
      _tick = Timer.periodic(const Duration(seconds: 1), (_) {
        if (!mounted) return;
        setState(() {});
        if (!widget.store.applianceDemoActive) {
          _tick?.cancel();
          _tick = null;
        }
      });
    } else if (!active && _tick != null) {
      _tick?.cancel();
      _tick = null;
    }
  }

  @override
  Widget build(BuildContext context) {
    final store = widget.store;
    final active = store.applianceDemoActive;
    _syncTimer(active);
    final left = active
        ? store.applianceDemoUntil!.difference(DateTime.now()).inSeconds + 1
        : 0;
    final enabled = store.link == Link.ok && !store.applianceDemoBusy;
    return Row(mainAxisSize: MainAxisSize.min, children: [
      if (active) ...[
        Text('가전 시연 중 · $left초',
            style: Typo.sans(16, weight: 600, color: Palette.lamp)),
        const SizedBox(width: 10),
      ],
      NuniButton(
          key: const ValueKey('appliance-demo'),
          label: active ? '시연 끝내기' : '가전 시연 ${NuniStore.applianceDemoSeconds}초',
          size: 15,
          onPressed: enabled
              ? () => store.setApplianceDemo(
                  active ? 0 : NuniStore.applianceDemoSeconds)
              : null),
    ]);
  }
}
