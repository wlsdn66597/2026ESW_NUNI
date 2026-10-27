# NUNI 설치 안내

## 유선 연결

Pi 4의 환경 ESP·MR60·가전 ESP 2대는 각각 USB로 연결한다. Pi 4와 Pi 5는 Ethernet으로 연결한다. 환경 ESP의 SDA/SCL은 GPIO21/22, 릴레이 입력은 GPIO23이다. 릴레이 모듈의 논리·코일 전원은 모듈 규격에 맞춰 공급하고 USB 5V 부하 전원은 접점으로 스위칭한다.

## 서비스

1. atlas C++ 빌드 결과를 서비스 패키지에 넣고 Pi 5에 설치한다.
2. Pi 5에서 `setup_on_device.sh`를 실행해 D-Bus·systemd 서비스를 등록한다.
3. `setup_direct_lan.sh`로 Pi 4와 Pi 5의 주소를 각각 설정한다.
4. `bin/`과 `deploy/`로 구성한 배포 묶음에서 `install_wired_role.sh pi4`와 `install_wired_role.sh pi5`를 각 장치에서 실행한다. 배포 묶음에는 `nunisensors`, `nunivibe`, `nunifusion`과 deploy 설정을 배치한다.
5. `appliance_bridge/deploy/install.sh`로 Python ARM64 배포 묶음을 설치하고 `enroll_usb.sh`로 환경·가전 ESP의 실제 포트와 식별값을 등록한다.
6. Flutter IPK를 Pi 5에 설치한다. `nuni-dashboard-start.service`가 부팅 때 대시보드를 실행한다.

USB 포트 등록은 `appliance_bridge/enroll.py`가 각 보드의 JSON 식별 프레임을 읽어 수행한다. 가전 장치는 보드의 식별값을 확인한 뒤 명령을 전달한다.

## 모델과 음성

`atlas/nuni/files/model/`의 YAMNet float32 모델과 맥락 모델은 서비스 번들에 함께 설치된다. YAMNet 변환 도구는 `cry_model/export_yamnet_tflite.py`에 있다. `files/voice/parent.wav`는 한국어 기본 TTS이며, 초기 설치 시 `/data/nuni/voice/parent.wav`로 복사된다. 음성 경로는 `NUNI_SOOTHE_WAV`로 지정할 수 있다. 마이크 입력 보정은 `65-mic-input-gain.conf`가 적용한다.

## 가습기 명령

Pi 5의 환경 정책은 `control/humidifier/command`를 발행한다. Pi 4는 현재 환경 표본, ESP 부팅 식별값, 중복 명령과 릴레이 응답을 확인한 뒤 같은 환경 ESP USB 포트로 `NUNIH1` 명령을 보낸다. ESP는 GPIO23을 제어하고 `NUNI1`의 `relay` 프레임으로 응답한다. 릴레이 명령은 4초의 사용 기한을 가지며 주기적으로 갱신한다.

## ThinQ

`probe_thinq.py`는 SDK 어댑터의 명령 경로를 실행한다. `THINQ_PAT` 환경변수를 설정하고 `--list`로 계정의 가전 목록을 조회한다. 에어컨·공기청정기 LED 시연은 `thinq_sdk_mock` 모드와 USB 명령 게이트웨이를 사용한다.
