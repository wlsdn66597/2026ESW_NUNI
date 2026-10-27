# NUNI appliance bridge

Pi 5의 ThinQ SDK 명령 흐름·온습도 정책과 Pi 4의 USB 가전 게이트웨이. 에어컨·공기청정기 ESP의 LED 상태와 환경 ESP의 가습기 릴레이를 연결한다.

```sh
python -m pip install -r appliance_bridge/requirements.txt
python -m unittest discover -s appliance_bridge/tests -v
python appliance_bridge/probe_thinq.py
python appliance_bridge/policy.py
```

SDK 시연 모드는 `thinq_sdk_mock`이다. 가습기 명령은 `local_relay` 모드로 발행한다. `probe_thinq.py --list`는 `THINQ_PAT` 환경변수로 가전 목록을 조회한다.
