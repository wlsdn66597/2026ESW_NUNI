# NUNI 기록 DB와 실제 기록 화면

2026-11-01. WSL Ubuntu-22.04의 `/home/lady6/atlas/atlas_docker_env/atlas_docker_env/nuni`, `feat#1` 작업 사본.

## 구현 상태

- 방 상태(온도·습도·CO₂)는 **1시간(설정 시 2시간) 단위 요약**만 저장한다. 같은 시간에 레이더가 아기를 본 비율도 센다.
- 울음 기록(시작·끝·보호자 답 = 무엇으로 그쳤는지)과 **울음이 시작될 때의 방 상태**를 저장한다.
- 경보·난간 감지·부모 목소리·진동·가전 켜짐/꺼짐 같은 사건은 **상태가 바뀔 때만** 저장한다. 1~2초마다 반복되는 제어 메시지(control/action, 가습기 명령·상태)는 내용이 바뀐 순간만 남긴다.
- 레이더·마이크의 1분 통계와 1초 울음 판정은 저장하지 않는다(2026-11-05 결정, 용량·SD 쓰기 감소).
- 최근 7일 시간대별 울음 횟수, 최근 돌봄 기록, 아기가 있고 울지 않은 시간의 방 상태 범위를 DB에서 계산한다.
- Flutter 실제 탭은 D-Bus `GetHistorySummary`만 읽는다. 최근 40개 울음이나 앱 실행 시간에 의존해 통계를 만들지 않는다.
- 실제 기기의 기본 탭은 '실제'다. '예시'는 별도 합성 화면이며 저장·학습에 섞지 않는다.
- 기존 `cry_log.json`을 유지하면서 최근 보관 기간의 완료된 울음을 DB에 복사한다.
- 기존 DB(user_version=1·2)를 지우지 않고 user_version=3으로 업그레이드한다. 분 단위 환경 요약은 시간 단위로 합쳐 옮기고, sensor_minutes·quiet_environment 표는 지운다.

**미완료:** 실제 Pi에서의 수집·마이크·D-Bus 정책·재부팅·장기 용량 확인. 이번 작업은 실제 장치에 배포하지 않았다.
SQLite 정적 링크는 SDK에 없는 sqlite3 개발 라이브러리 문제를 해결한다. 기존 오디오·AI 런타임은 여전히 atlas 장치에 필요하다.

## 데이터 흐름

```text
sensor/env, sensor/radar(재실 비율만), cry/*, fusion/alert, event/tof, control/*
                        ↓
           nunifusion의 HistoryStore 작업 스레드
                        ↓
                  /data/nuni/nuni.db
                        ↓
         com.atlas.NuniFusion1 D-Bus 메서드
                        ↓
                  Flutter 실제 탭
```

Pi 5 안에서 생성한 울음·답·경보·제어 사건은 MQTT 발행 전에 기록 대기열에 넣는다.
브로커가 끊겨도 이 로컬 사건은 기록할 수 있다. MQTT로 되돌아온 같은 사건은 중복 제거한다.
Pi 4 등 외부 발행자의 메시지는 브로커를 통해 실제 수신한 것만 기록할 수 있다. QoS 0 통신 자체의 손실을 복구하지는 않는다.

## 저장 규칙

| 입력 | 테이블/내용 |
|---|---|
| sensor/env | environment_buckets: 1시간(또는 2시간)마다 유효 고유 측정의 온도·습도·CO₂ 합·최솟값·최댓값, 전체/유효 수 |
| sensor/radar | environment_buckets: 같은 시간의 레이더 발행 수(radar_count)와 재실 수(presence_count)만 |
| audio/cry | 저장하지 않음(울음 구간은 cry/*로 기록) |
| cry/*, fusion/alert, event/tof, control/* | recorded_events: 토픽·JSON·Pi 5 수신 시각·저장 ID. 반복 제어 토픽은 상태가 바뀔 때만 |
| 울음 시작·종료·drop·보호자 답 | cry_episodes: 같은 울음 ID의 상태와 최종 답, 시작 때 방 상태(room_temp·room_humidity·room_co2) |

- 시간 단위는 모두 **Unix 초**. 방 상태 요약 시작은 버킷 길이(3600/7200초)로 내림한다. API 범위는 `[from_s, to_s)`다.
- 울음 시작 때 방 상태는 2분 이내의 가장 최근 유효 측정을 붙인다. 없으면 null이다.
- 반복 제어 토픽(control/action, control/humidifier/command·state·policy)은 id·ts·seq·만료 시각 같은 매번 바뀌는 필드를 뺀 내용이 바뀔 때만 사건으로 남긴다.
- 사건의 중복 판별 키는 내용 전체 대신 `토픽:FNV-1a 64비트 해시`다.
- 환경은 ESP `sample_id`를 우선해 같은 측정의 반복 발행을 제외한다. 없으면 boot_id/seq, 그다음 ts를 사용한다.
- 레이더는 허브의 boot_id/seq로 같은 발행을 한 번만 센다. presence_valid가 참인 발행만 radar_count에 넣는다.
- 잘못된 숫자, invalid, 오래된 표본, retained 메시지를 정상 관측으로 세지 않는다.
- Pi 4 USB 데이터는 두 Pi의 시각 차이를 고려해 Pi 5 수신 시각과 age_s를 사용한다.
- 기록이 없던 시간은 0으로 만들지 않는다. 유효 환경 측정이 없는 시간의 통계는 null이다.
- 울음 횟수는 완료된 울음 구간 수다. 60초 동안 운 것을 60회로 세지 않고, drop된 짧은 잡음도 제외한다.
- 답이 먼저 도착해도 종료 사건과 같은 ID로 연결한다. 예전 답이 늦게 들어와 최신 답을 덮어쓰지 않게 answered_at을 비교한다.
- 사건의 키 순서는 정규화한다. ID/시각/boot+seq가 있는 같은 내용은 중복 제거한다. 식별 정보가 전혀 없는 동일 내용은 각각의 수신 사건으로 보존한다.

## 울음 없던 환경의 의미

조회할 때 environment_buckets에서 계산한다(따로 저장하지 않음). 다음을 모두 만족하는 시간만 쓴다.

- 유효 환경 측정이 있다.
- 레이더 발행이 있고, 그중 절반 이상에서 아기가 보였다(아기가 없는 낮 시간 제외).
- 그 시간과 겹치는 울음 구간(drop 제외)이 없다.

이런 시간이 3개 이상일 때 실제 최솟값·최댓값 범위를 제공한다(`quiet.count`는 시간 수, `quiet.bucket_seconds`는 버킷 길이).
이는 관측 기록이며 울음 원인이나 의학적 효과를 판단하는 기능이 아니다.

## 기존 기록과 복구

시작할 때 `NUNI_CRY_DIR/cry_log.json`을 읽어 보관 기간 안의 완료된 울음을 복사한다.
원본 JSON, WAV, 임베딩은 변경하지 않는다. JSON은 기존 후보 계산·답 저장의 기준으로 계속 사용한다.
같은 파일을 여러 번 읽어도 울음 ID가 중복되지 않고, DB의 더 최신 답을 예전 JSON으로 덮어쓰지 않는다.
읽을 수 없거나 잘못된 JSON은 로그에 오류를 남기고 기존 DB를 유지한다. 가져오기 한도는 32 MiB/최근 10080건이다.
JSON의 과거 울음을 인위적인 새 MQTT 사건으로 만들지는 않는다.
Flutter의 과거 care-history.json은 미전송 선택을 보존하는 로컬 파일로 유지하지만, 그 내용을 DB 실제 통계의 대체 자료로 사용하지 않는다.

## 운영 설정

| 환경변수 | 기본값 | 범위/의미 |
|---|---|---|
| NUNI_ENABLE_HISTORY | 1 | 0이면 기록 기능 끄기 |
| NUNI_HISTORY_DB | /data/nuni/nuni.db | 영구 데이터 경로 |
| NUNI_HISTORY_DAYS | 30 | 7~365일 |
| NUNI_HISTORY_MAX_MIB | 64 | DB 본체 상한, 4~1024 MiB |
| NUNI_HISTORY_ENV_BUCKET_S | 3600 | 방 상태 요약 단위, 3600(1시간) 또는 7200(2시간). 바꾸면 기존 요약을 새 단위로 합친다 |
| NUNI_HISTORY_FLUSH_S | 30 | 모아서 저장하는 간격, 1~600초 |

64 MiB·30일은 초기 기본값이다. 최종 운영값은 실제 `/data` 여유 공간과 하루 증가량으로 확정해야 한다.
DB 초기화 실패 시 로그를 남기고 기존 센서 서비스는 계속 실행하며, 조회는 오류를 반환한다.
대기열 초과로 버린 입력 수는 조회의 dropped_samples에서 확인한다.

일반 표본은 작업 스레드가 처리하며 NUNI_HISTORY_FLUSH_S(기본 30초)마다 한 번에 모아 저장한다. 조회·정상 종료 시에도 남은 자료를 저장한다.
WAL/NORMAL 동기화를 사용한다. DB가 깨지지는 않지만 전원 차단 시 마지막 저장 이후(최대 약 30초) 자료는 잃을 수 있다. 체크포인트는 자동에 맡긴다.
사건 폭주는 작은 트랜잭션으로 나눠 저장한다. DB 잠김·쓰기 오류는 로그와 조회 오류로 드러나며 재시도한다.
대기열 4096개, 시간별 집계와 소스 체크포인트에도 메모리 한도가 있으므로 장기 디스크 장애에서 완전 무손실을 보장하지 않는다.

보관 기간보다 용량 상한에 먼저 도달하면 큰 원본 사건 로그부터 오래된 부분을 정리한다.
이후 필요하면 방 상태 요약, 보호자 답 순으로 정리해 통계를 최대한 보호한다.
해당 조회 범위의 자료가 용량 때문에 정리됐다면 limited_by_size=true를 반환한다.
실제 탭 통계가 영향을 받으면 사용자에게 일부 이전 기록이 정리됐음을 표시한다.
따라서 '최근 7일'은 조회 기간이며, 공간 부족으로 삭제된 기록까지 복구해 완전한 7일을 보장한다는 뜻은 아니다.
WAL/SHM와 기존 JSON·WAV·임베딩은 DB 본체 상한에 포함되지 않는다. 이미 큰 DB에 더 작은 상한을 적용하려면 백업·축소 계획이 별도로 필요하다.

## D-Bus API

이름/인터페이스 `com.atlas.NuniFusion1`, 경로 `/com/atlas/NuniFusion1`.
모든 반환형은 JSON 문자열 `s`. 오류는 `{"ok":false,"error":"..."}`.
공통 시간 범위는 최대 31일, 시작<종료, 음수가 아닌 Unix 초다.

| 메서드 | 입력 서명 | 내용 |
|---|---|---|
| GetHistorySummary | xxu | 시작·종료·최근 울음 목록 최대 개수 |
| GetEnvironmentHistory | xxu | 시작·종료·최대 요약 개수 |
| GetRecordedEvents | xxux | 시작·종료·최대 사건 수·이전 after_id |

`x`는 int64, `u`는 uint32. events limit은 1~1000, 나머지는 1~10080.
Flutter는 최근 7일과 목록 limit=1000을 요청한다. hourly/total_count는 목록 제한과 관계없이 기간 전체에서 계산한다.
시간대별 통계와 실제 기록 날짜 표시는 Asia/Seoul 기준이다. 시작 시각으로 횟수를 센다.
목록은 최근 울음부터이며, 실제 화면의 돌봄 답 입력은 기존 AnswerCry를 그대로 사용한다.

Summary 예:

```json
{"ok":true,"time_unit":"unix_seconds","timezone":"Asia/Seoul","total_count":61,"hourly":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,60,1,0,0,0,0],"episodes":[{"id":"example","t0":1793460000,"t1":1793460090,"action":"feed","room":{"temp":23.1,"humidity":34,"co2":820}}],"truncated":true,"quiet":{"count":3,"bucket_seconds":3600,"temperature_c":{"min":22.1,"max":24.4},"humidity_pct":{"min":46,"max":53},"co2_ppm":{"min":500,"max":700}},"limited_by_size":false}
```

Environment 결과는 bucket_seconds와 records(bucket_s,total_count,valid_count, temperature_c/humidity_pct/co2_ppm의 mean/min/max, radar_count, presence_ratio)를 담는다. 다음 페이지는 마지막 bucket_s+bucket_seconds부터 요청한다.
Events 결과는 records에 id,topic,payload,received_s를 담는다. 저장 ID 순서이며 after_id로 다음 페이지를 요청한다.

## Flutter 동작

DbusStore가 GetHistorySummary를 1분마다, 그리고 새 울음 사건(CryEpisode 신호)이나 보호자 답 직후에 조회하고, HistoryView가 결과를 검증해 표시한다.
로드 중·실패·정상적인 빈 결과를 구분한다. 처음 실패했을 때 0회라고 단정하거나 합성 기록으로 채우지 않는다.
이전 조회 후 연결이 끊기면 이전 자료임을 표시하고 복구 후 새 결과로 바꾼다.
앱이 닫혀도 기록은 서비스에서 계속 쌓인다.
NUNI_SAMPLE_HISTORY=true를 빌드 시 지정하면 의도적으로 예시 탭부터 열 수 있다.
새 앱은 새 백엔드와 함께 배포해야 한다. 구형 서비스에는 GetHistorySummary가 없어 기록 연결 오류가 표시된다.

## PC 빌드·시험

Windows PowerShell에서 `wsl -d Ubuntu-22.04` 실행 후:

```sh
cd ~/atlas/atlas_docker_env/atlas_docker_env/nuni
cmake -S atlas/nuni -B build/native
cmake --build build/native --parallel 2
ctest --test-dir build/native --output-on-failure
build/native/environment_history_demo build/native/environment-demo.db
```

데모는 가상 23/24/25도를 저장하고 DB를 다시 열어 평균 24도를 조회한다. 실제 수집 자료가 아니다.
C++ 7개 테스트는 기존 센서 검사와 집계·중복·보관 기간·동시 접근·DB 잠김·마이그레이션·JSON 복사·크기 제한을 포함한다.
실제 nunifusion에 시험 MQTT 패킷을 넣어 환경·레이더·울음·답·사건 저장을 확인한다.

```sh
~/atlas/dev.sh "cd /app/nuni/atlas/dashboard && flutter-atlas test"
~/atlas/dev.sh "cd /app/nuni && ATLAS_SDK_ROOT=/opt/atlas-sdk-x86_64 NUNI_BUILD_DIR=/app/nuni/build/atlas NUNI_BUILD_JOBS=2 bash atlas/nuni/deploy/build_with_local_sdk.sh"
~/atlas/dev.sh "cd /app/nuni/atlas/dashboard && flutter-atlas build atlas --release --ipk"
```

Flutter 19개 테스트에는 전체 통계와 최근 목록의 구분, 시간 단위, 연결 실패·복구, 중복 조회 방지, 실제 탭의 표시가 포함된다.
설치 패키지: `atlas/dashboard/build/atlas/arm64/release/ipk/com.atlas.app.nuni_dashboard.ipk`.

## 실제 D-Bus 형식 시험: Pi 없이

SDK QEMU로 ARM64 시험 서버를 실행하고 실제 세션 버스에서 메서드를 호출한다.
시험 서버는 오디오/AI를 사용하지 않으며 설치 대상에 포함되지 않는다.
SDK에 빠진 libcap/libzstd는 공식 Ubuntu ARM64 패키지를 해시 검증해 build/qemu-runtime에만 푼다.
이 보조 라이브러리는 시험 전용이며 SDK·IPK·Pi에 설치하지 않는다. 실제 장치 런타임과 동일하다는 의미는 아니다.

```sh
python3 atlas/nuni/tools/prepare_qemu_runtime.py
~/atlas/dev.sh "cd /app/nuni && dbus-run-session -- python3 atlas/nuni/tests/history_dbus_test.py --server /app/nuni/build/atlas/history_dbus_replay --sysroot /opt/atlas-sdk-x86_64/sysroots/armv8a-atlas-linux --extra-libs /app/nuni/build/qemu-runtime/usr/lib/aarch64-linux-gnu"
```

Summary·Environment·Events의 실제 입력/반환 서명, 페이지 커서, 잘못된 범위 처리를 확인한다.

## 실기기 배포 전 확인 순서

1. 팀 핫스팟과 SSH 키 등록을 확인하고 현재 Pi 5 주소를 확인한다. 172.20.10.13은 인수인계 당시 주소다.
2. Pi 5에서 `df -h /data`, `systemctl cat nunifusion`, `systemctl status nunifusion --no-pager`를 읽는다.
3. JSON·WAV·기존 DB 크기를 확인해 NUNI_HISTORY_DAYS/MAX_MIB 운영값을 정한다.
4. 서비스를 정상 종료한 뒤 기존 바이너리와 JSON·DB를 백업한다. 실행 중 DB 본체만 복사하면 WAL 내용이 빠질 수 있다.
5. 현재 서비스 경로·라이브러리 구성을 유지하며 새 ARM64 바이너리로 교체한다. 전체 wired 설치 스크립트로 시연 장비의 임시 Wi-Fi 경로를 덮어쓰지 않는다.
6. 서비스를 시작하고 `[history]`와 JSON import 로그를 확인한다. DB는 `/data`에 두므로 데이터 저장을 위해 루트를 rw로 바꿀 필요가 없다.
7. ARC/기존 팀 배포 절차로 새 대시보드 IPK를 설치한다. 새 앱·새 서비스를 함께 확인한다.
8. 실제 센서·울음 종료·보호자 답을 입력하고 실제 탭과 DB가 일치하는지 확인한다.
9. 앱만 꺼도 수집이 계속되는지, 아기가 없을 때(레이더 재실 없음) 평소 방 상태 범위에 들어가지 않는지 확인한다.
10. 서비스 재시작과 Pi 재부팅 후 자료 보존·자동 실행을 확인하고, 하루 증가량과 삭제 정책을 점검한다.

원본 JSON은 기존 학습·돌봄 기능을 위해 유지한다. 원본 기록 삭제나 실제 장치 배포는 이번 PC 작업에서 수행하지 않았다.
