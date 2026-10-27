// 화면 문구와 한국어 도우미(조사·시각·시간 길이).
// 명조체(상태 문장·질문)는 이 파일과 화면 코드에 적힌 글자만 담아 앱을 가볍게 한다(tool/subset_fonts.py).

/// 받침이 있으면 [withBatchim], 없으면 [without] (예: 수유는 · 기저귀는 · 울음은)
String josa(String word, String withBatchim, String without) {
  if (word.isEmpty) return without;
  final c = word.codeUnitAt(word.length - 1);
  if (c < 0xAC00 || c > 0xD7A3) return without;
  return (c - 0xAC00) % 28 != 0 ? withBatchim : without;
}

String eunNeun(String w) => '$w${josa(w, '은', '는')}';
String iGa(String w) => '$w${josa(w, '이', '가')}';
String eulReul(String w) => '$w${josa(w, '을', '를')}';

/// 오전 1:58
String clock(DateTime t) {
  final h = t.hour % 12 == 0 ? 12 : t.hour % 12;
  final m = t.minute.toString().padLeft(2, '0');
  return '${t.hour < 12 ? '오전' : '오후'} $h:$m';
}

/// 1:58 (오전·오후 없이, 목록용)
String clockShort(DateTime t) {
  final h = t.hour % 12 == 0 ? 12 : t.hour % 12;
  return '$h:${t.minute.toString().padLeft(2, '0')}';
}

/// 3분 · 1분 12초 · 40초 · 2시간 10분
String span(Duration d, {bool seconds = true}) {
  final s = d.inSeconds.abs();
  if (s < 60) return seconds ? '$s초' : '1분 미만';
  final h = s ~/ 3600, m = (s % 3600) ~/ 60, sec = s % 60;
  if (h > 0) return m > 0 ? '$h시간 $m분' : '$h시간';
  if (seconds && sec > 0 && m < 10) return '$m분 $sec초';
  return '$m분';
}

/// 2시간 10분 전 · 방금
String ago(DateTime t, DateTime now) {
  final d = now.difference(t);
  if (d.inSeconds < 60) return '방금';
  return '${span(d, seconds: false)} 전';
}

/// 답 버튼에 쓰는 말(기록은 서비스 이름표 그대로: 수유 · 기저귀 · 안아주기·자세 바꾸기 …)
const answerButtonText = {
  'feed': '수유',
  'diaper': '기저귀',
  'hold': '안아주기·자세 바꾸기',
  'self': '저절로 그쳤어요',
  'sick': '아파 보여요',
  'other': '기타',
  'unknown': '모르겠어요',
};

/// 환경 권고(fusion/state actions)
const roomAdvice = {
  'humidify': '습도가 낮아요. 가습기를 켜 두면 좋아요.',
  'dehumidify': '습도가 높아요. 잠깐 환기해 주세요.',
  'ventilate': '이산화탄소가 높아요. 창을 잠깐 열어 주세요.',
  'cool': '방이 더워요. 온도를 조금 낮춰 주세요.',
  'heat': '방이 서늘해요. 온도를 조금 올려 주세요.',
  'dim_light': '방 밝기를 확인하고 조명을 조절해 주세요.',
};

/// 경보 이유(서비스 문구 → 부모에게 보여 줄 말). 모르는 이유는 그대로 보인다.
String reasonText(String r) {
  if (r.startsWith('호흡 신호 이상') || r.startsWith('호흡 미검출'))
    return '호흡 신호를 확인해 주세요';
  if (r.startsWith('울음 감지')) return '울음이 이어지고 있어요';
  if (r.startsWith('다중 신호')) return '울음과 다른 센서 신호를 함께 확인해 주세요';
  if (r.startsWith('경계 호흡')) return '호흡 신호와 다른 센서 신호를 함께 확인해 주세요';
  if (r.startsWith('CO₂') || r.startsWith('CO2')) return '이산화탄소가 높아요';
  if (r.startsWith('실내 온도')) return '방 온도가 알맞지 않아요';
  if (r.startsWith('무호흡')) return '호흡 신호를 확인해 주세요';
  if (r.startsWith('움직임')) return '아기 상태를 직접 확인해 주세요';
  return r;
}
