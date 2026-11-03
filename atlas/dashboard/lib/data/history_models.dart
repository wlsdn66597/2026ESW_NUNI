class CareRecord {
  const CareRecord(this.id, this.at, this.seconds, this.action);
  final String id;
  final DateTime at;
  final int seconds;
  final String? action;
  Map<String, dynamic> toJson() => {
        'id': id,
        'at': at.toIso8601String(),
        'seconds': seconds,
        'action': action
      };
  factory CareRecord.fromJson(Map<String, dynamic> j) => CareRecord(
      j['id'] as String,
      DateTime.parse(j['at'] as String),
      j['seconds'] as int,
      j['action'] as String?);
}

class EnvironmentRecord {
  const EnvironmentRecord(
      this.at, this.temp, this.humidity, this.co2, this.quietMinutes);
  final DateTime at;
  final double temp, humidity, co2;
  final int quietMinutes;
  Map<String, dynamic> toJson() => {
        'at': at.toIso8601String(),
        'temp': temp,
        'humidity': humidity,
        'co2': co2,
        'quiet': quietMinutes
      };
  factory EnvironmentRecord.fromJson(Map<String, dynamic> j) =>
      EnvironmentRecord(
          DateTime.parse(j['at'] as String),
          (j['temp'] as num).toDouble(),
          (j['humidity'] as num).toDouble(),
          (j['co2'] as num).toDouble(),
          j['quiet'] as int);
}

class HistoryView {
  const HistoryView(this.records, this.environment,
      {this.sample = false,
      this.database = false,
      this.available = true,
      this.limitedBySize = false,
      this.hourlyCounts,
      this.totalRecords,
      this.quietRecords,
      this.temperatureMin,
      this.temperatureMax,
      this.humidityMin,
      this.humidityMax,
      this.loading = false,
      this.error});
  final List<CareRecord> records;
  final List<EnvironmentRecord> environment;
  final bool sample, database, loading, available, limitedBySize;
  final String? error;
  final List<int>? hourlyCounts;
  final int? totalRecords, quietRecords;
  final double? temperatureMin, temperatureMax, humidityMin, humidityMax;
  int get totalCount => totalRecords ?? records.length;
  int get quietCount => quietRecords ?? quiet.length;

  HistoryView withStatus({bool loading = false, String? error}) =>
      HistoryView(records, environment,
          sample: sample,
          database: database,
          available: available,
          limitedBySize: limitedBySize,
          hourlyCounts: hourlyCounts,
          totalRecords: totalRecords,
          quietRecords: quietRecords,
          temperatureMin: temperatureMin,
          temperatureMax: temperatureMax,
          humidityMin: humidityMin,
          humidityMax: humidityMax,
          loading: loading,
          error: error);

  factory HistoryView.fromDatabase(Map<String, dynamic> j) {
    if (j['ok'] != true ||
        j['time_unit'] != 'unix_seconds' ||
        j['timezone'] != 'Asia/Seoul') {
      throw const FormatException('Invalid history response');
    }
    final rawHours = j['hourly'];
    final total = j['total_count'];
    final rawEpisodes = j['episodes'];
    final quiet = j['quiet'];
    if (rawHours is! List ||
        rawHours.length != 24 ||
        rawHours.any((v) => v is! int || v < 0) ||
        total is! int ||
        total < 0 ||
        rawEpisodes is! List ||
        quiet is! Map ||
        quiet['count'] is! int ||
        quiet['count'] < 0) {
      throw const FormatException('Invalid history summary');
    }
    final hours = List<int>.from(rawHours);
    if (hours.fold<int>(0, (sum, v) => sum + v) != total ||
        rawEpisodes.length > total) {
      throw const FormatException('Inconsistent history count');
    }
    final records = <CareRecord>[];
    for (final value in rawEpisodes) {
      if (value is! Map ||
          value['id'] is! String ||
          (value['id'] as String).isEmpty ||
          value['t0'] is! num ||
          value['t1'] is! num ||
          (value['action'] != null && value['action'] is! String)) {
        throw const FormatException('Invalid cry record');
      }
      final start = (value['t0'] as num).toDouble(),
          end = (value['t1'] as num).toDouble();
      if (!start.isFinite ||
          !end.isFinite ||
          start < 0 ||
          end < start ||
          end > 253402300799) {
        throw const FormatException('Invalid cry timestamp');
      }
      records.add(CareRecord(
          value['id'] as String,
          DateTime.fromMillisecondsSinceEpoch((start * 1000).round()),
          (end - start).floor(),
          value['action'] as String?));
    }
    (double?, double?) range(String name) {
      final value = quiet[name];
      if (value == null) return (null, null);
      if (value is! Map || value['min'] is! num || value['max'] is! num) {
        throw const FormatException('Invalid environment range');
      }
      final lo = (value['min'] as num).toDouble(),
          hi = (value['max'] as num).toDouble();
      if (!lo.isFinite || !hi.isFinite || lo > hi || quiet['count'] < 3) {
        throw const FormatException('Invalid environment range');
      }
      return (lo, hi);
    }

    final temp = range('temperature_c'), humidity = range('humidity_pct');
    return HistoryView(List.unmodifiable(records), const [],
        database: true,
        limitedBySize: j['limited_by_size'] == true,
        hourlyCounts: List.unmodifiable(hours),
        totalRecords: total,
        quietRecords: quiet['count'] as int,
        temperatureMin: temp.$1,
        temperatureMax: temp.$2,
        humidityMin: humidity.$1,
        humidityMax: humidity.$2);
  }
  List<int> get hourly {
    if (hourlyCounts != null) return List.of(hourlyCounts!);
    final counts = List<int>.filled(24, 0);
    for (final r in records) {
      counts[r.at.hour]++;
    }
    return counts;
  }

  int get peakBlock {
    final h = hourly;
    var best = 0, peak = 0;
    for (var b = 0; b < 8; b++) {
      final n = h.sublist(b * 3, b * 3 + 3).fold(0, (a, v) => a + v);
      if (n > peak) {
        best = b;
        peak = n;
      }
    }
    return best;
  }

  static const blockLabels = [
    '자정–새벽 3시',
    '새벽 3–6시',
    '오전 6–9시',
    '오전 9시–정오',
    '정오–오후 3시',
    '오후 3–6시',
    '오후 6–9시',
    '오후 9시–자정'
  ];
  List<EnvironmentRecord> get quiet =>
      environment.where((e) => e.quietMinutes >= 10).toList();
  String? get temperatureRange {
    if (database) {
      return temperatureMin == null
          ? null
          : '${temperatureMin!.floor()}–${temperatureMax!.ceil()}℃';
    }
    if (quiet.length < 3) return null;
    final values = quiet.map((e) => e.temp).toList()..sort();
    return '${values.first.floor()}–${values.last.ceil()}℃';
  }

  String? get humidityRange {
    if (database) {
      return humidityMin == null
          ? null
          : '${(humidityMin! / 5).floor() * 5}–${(humidityMax! / 5).ceil() * 5}%';
    }
    if (quiet.length < 3) return null;
    final values = quiet.map((e) => e.humidity).toList()..sort();
    return '${(values.first / 5).floor() * 5}–${(values.last / 5).ceil() * 5}%';
  }
}
