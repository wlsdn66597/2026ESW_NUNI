cmake_minimum_required(VERSION 3.16)
project(nunifusion VERSION 0.1.0 LANGUAGES CXX)

# 숨결지기 atlas 네이티브 서비스(com.atlas.NuniFusion1)
#
# 개발 컨테이너(atlas SDK 환경)에서:  mkdir build && cd build && cmake .. && make
#   - TFLite(울음 감지), libpulse(마이크), sdbus-c++(D-Bus)는 SDK sysroot에서 찾는다.
#   - 하나라도 없으면 그 기능만 빼고 빌드한다. 제출용 빌드는 -DNUNI_REQUIRE_ALL=ON으로 빠짐없이 확인한다.
# 개발 PC(일반 리눅스)에서: 같은 명령으로 빌드되며, 없는 라이브러리 기능만 빠진다. 시험은 tests/ 참고.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# 기기 시스템 경로를 건드리지 않도록 실행 파일 옆 lib/의 공유 라이브러리를 먼저 찾는다(atlas/yamnet_test와 같음)
set(CMAKE_BUILD_RPATH "$ORIGIN/lib")
set(CMAKE_INSTALL_RPATH "$ORIGIN/lib")
# ARM(aarch64)에서 컴파일러가 a*b+c를 FMA 한 번으로 합치면 마지막 자리가 x86·Python과 달라진다.
# 노트북 Python 결과와 같은 값을 내도록 합치지 않게 한다(qemu로 aarch64 비교 시험에서 확인).
add_compile_options(-ffp-contract=off)

option(NUNI_REQUIRE_ALL "TFLite·libpulse·sdbus-c++ 중 하나라도 없으면 오류" OFF)
set(NUNI_TFLITE_INCLUDE_DIRS "" CACHE STRING "TFLite 헤더 경로(pkg-config 대신 직접 지정할 때)")
set(NUNI_TFLITE_LIBS "" CACHE STRING "TFLite 라이브러리(pkg-config 대신 직접 지정할 때)")

find_package(PkgConfig QUIET)
find_package(Threads REQUIRED)

# ── 핵심 로직(외부 의존성 없음) ──────────────────────────────────────────────
add_library(nuni_core STATIC
  src/json_util.cpp
  src/sleep_state.cpp
  src/env_control.cpp
  src/personalization.cpp
  src/fusion.cpp
  src/radar_bridge.cpp
  src/cry_detector.cpp
  src/cry_log.cpp
  src/tof_watch.cpp
  src/sensor_hub.cpp
  src/mqtt.cpp
  src/audio_wav.cpp
)
target_include_directories(nuni_core PUBLIC include third_party)
target_link_libraries(nuni_core PUBLIC Threads::Threads)
target_compile_options(nuni_core PRIVATE -Wall -Wextra)

# ── TFLite (울음 감지) ───────────────────────────────────────────────────────
set(HAVE_TFLITE OFF)
if(NUNI_TFLITE_LIBS)
  set(HAVE_TFLITE ON)
  set(TFLITE_INCLUDE_DIRS ${NUNI_TFLITE_INCLUDE_DIRS})
  set(TFLITE_LINK ${NUNI_TFLITE_LIBS})
elseif(PkgConfig_FOUND)
  pkg_check_modules(TFLITE QUIET tensorflowlite)
  if(NOT TFLITE_FOUND)
    pkg_check_modules(TFLITE QUIET tensorflow2-lite)
  endif()
  if(TFLITE_FOUND)
    set(HAVE_TFLITE ON)
    # pkg-config 목록에는 GPU 위임(EGL·GLES)까지 있다. CPU(XNNPACK)만 쓰므로 필요한 것만 링크한다
    set(TFLITE_LINK tensorflowlite tensorflowlite_xnnpack_delegate)
  endif()
endif()
if(HAVE_TFLITE)
  add_library(nuni_tflite STATIC src/yamnet_tflite.cpp)
  target_include_directories(nuni_tflite PUBLIC ${TFLITE_INCLUDE_DIRS})
  target_link_directories(nuni_tflite PUBLIC ${TFLITE_LIBRARY_DIRS})
  target_link_libraries(nuni_tflite PUBLIC nuni_core ${TFLITE_LINK} ${CMAKE_DL_LIBS})
  target_compile_definitions(nuni_tflite PUBLIC NUNI_WITH_TFLITE)
endif()

# ── ECO.AI 통합 API (울음 감지, Pi 5) ────────────────────────────────────────
# Pi 5 이미지에는 추론 플러그인이 있어 ECO.AI로 돌린다. 플러그인이 없는 기기(Pi 4)에서는 실행 때 TFLite 직접으로 넘어간다.
set(HAVE_ECOAI OFF)
if(PkgConfig_FOUND)
  pkg_check_modules(ECOAI QUIET ecoai-inference)
  if(ECOAI_FOUND)
    set(HAVE_ECOAI ON)
    add_library(nuni_ecoai STATIC src/yamnet_ecoai.cpp)
    target_include_directories(nuni_ecoai PUBLIC ${ECOAI_INCLUDE_DIRS})
    target_link_directories(nuni_ecoai PUBLIC ${ECOAI_LIBRARY_DIRS})
    target_link_libraries(nuni_ecoai PUBLIC nuni_core ${ECOAI_LIBRARIES})
    target_compile_definitions(nuni_ecoai PUBLIC NUNI_WITH_ECOAI)
  endif()
endif()

# ── libpulse (마이크) ────────────────────────────────────────────────────────
set(HAVE_PULSE OFF)
if(PkgConfig_FOUND)
  pkg_check_modules(PULSE QUIET libpulse)
  if(PULSE_FOUND)
    set(HAVE_PULSE ON)
  endif()
endif()

# ── sdbus-c++ 2.x (D-Bus 서비스) ─────────────────────────────────────────────
find_package(sdbus-c++ 2.0 QUIET)
if(sdbus-c++_FOUND)
  set(HAVE_SDBUS ON)
else()
  set(HAVE_SDBUS OFF)
endif()

if(NUNI_REQUIRE_ALL AND NOT (HAVE_TFLITE AND HAVE_PULSE AND HAVE_SDBUS))
  message(FATAL_ERROR "필요한 라이브러리가 없음: TFLite=${HAVE_TFLITE} libpulse=${HAVE_PULSE} sdbus-c++=${HAVE_SDBUS}")
endif()
message(STATUS "nunifusion 기능: ECO.AI(울음)=${HAVE_ECOAI} TFLite(울음)=${HAVE_TFLITE} libpulse(마이크)=${HAVE_PULSE} sdbus-c++(D-Bus)=${HAVE_SDBUS}")

# ── 서비스 실행 파일 ─────────────────────────────────────────────────────────
add_executable(nunifusion apps/nunifusion.cpp)
target_link_libraries(nunifusion PRIVATE nuni_core)
if(HAVE_TFLITE)
  target_link_libraries(nunifusion PRIVATE nuni_tflite)
endif()
if(HAVE_ECOAI)
  target_link_libraries(nunifusion PRIVATE nuni_ecoai)
endif()
if(HAVE_PULSE)
  target_sources(nunifusion PRIVATE src/audio_pulse.cpp)
  target_include_directories(nunifusion PRIVATE ${PULSE_INCLUDE_DIRS})
  target_link_directories(nunifusion PRIVATE ${PULSE_LIBRARY_DIRS})
  target_link_libraries(nunifusion PRIVATE ${PULSE_LIBRARIES})
else()
  target_sources(nunifusion PRIVATE src/audio_pulse_stub.cpp)
endif()
if(HAVE_SDBUS)
  target_sources(nunifusion PRIVATE src/dbus_service.cpp)
  target_link_libraries(nunifusion PRIVATE SDBusCpp::sdbus-c++)
  target_compile_definitions(nunifusion PRIVATE NUNI_WITH_DBUS)
endif()

# ── 파이 4 진동 노드(fusion/alert → PeripheralManager1 GPIO) ────────────────
add_executable(nunivibe apps/nunivibe.cpp)
add_executable(nunisensors apps/nunisensors.cpp)
target_link_libraries(nunisensors PRIVATE nuni_core)
target_link_libraries(nunivibe PRIVATE nuni_core)
if(HAVE_SDBUS)
  target_link_libraries(nunivibe PRIVATE SDBusCpp::sdbus-c++)
  target_compile_definitions(nunivibe PRIVATE NUNI_WITH_DBUS)
endif()

# ── 시험 도구(Python 원본과 출력 비교) ───────────────────────────────────────
add_executable(cry_log_replay tools/cry_log_replay.cpp)
target_link_libraries(cry_log_replay PRIVATE nuni_core)

add_executable(nuni_replay tools/nuni_replay.cpp)
target_link_libraries(nuni_replay PRIVATE nuni_core)
if(HAVE_TFLITE)
  target_link_libraries(nuni_replay PRIVATE nuni_tflite)
endif()

add_executable(wired_test tests/wired_test.cpp)
add_executable(usb_serial_test tests/usb_serial_test.cpp)
target_link_libraries(usb_serial_test PRIVATE nuni_core)
target_compile_options(usb_serial_test PRIVATE -UNDEBUG)
target_link_libraries(wired_test PRIVATE nuni_core)
target_compile_options(wired_test PRIVATE -UNDEBUG)
add_executable(mqtt_keepalive_test tests/mqtt_keepalive_test.cpp)
target_link_libraries(mqtt_keepalive_test PRIVATE nuni_core)
target_compile_options(mqtt_keepalive_test PRIVATE -UNDEBUG)

# ── 설치(= atlas 서비스 번들 구조, LG 샘플 lifecycleservice와 같음) ──────────
# arc build가 이 install 규칙으로 IPK를 만든다. 기기에서는 /data/share/usr/atlas/services/<id>/ 아래에 풀린다.
set(SVC_ID com.atlas.NuniFusion1)
set(SVC_DIR ${CMAKE_INSTALL_PREFIX}/services/${SVC_ID})
install(TARGETS nunifusion DESTINATION ${SVC_DIR})
install(FILES files/serviceinfo.json files/.apptype DESTINATION ${SVC_DIR})
install(FILES files/dbus/${SVC_ID}.conf DESTINATION ${CMAKE_INSTALL_PREFIX}/share/dbus-1/system.d)
install(FILES files/dbus/${SVC_ID}.service DESTINATION ${CMAKE_INSTALL_PREFIX}/share/dbus-1/system-services)
# 기기에서 마무리할 때 쓰는 파일(deploy/setup_on_device.sh가 시스템 경로로 옮긴다)
install(FILES deploy/nunifusion.service deploy/nuni-mosquitto.service deploy/nuni-dashboard-start.service DESTINATION ${SVC_DIR}/systemd)
install(PROGRAMS deploy/setup_on_device.sh DESTINATION ${SVC_DIR})
# 모델·음성·런타임 라이브러리·브로커를 서비스 번들에 함께 배치한다.
#   files/model/  : yamnet_emb.tflite, yamnet_class_map.csv
#   files/lib/    : libtensorflowlite.so, libtensorflowlite_xnnpack_delegate.so (기기에 없음)
#   files/mosquitto/ : build_mosquitto.sh 결과(mosquitto, mosquitto.conf)
install(DIRECTORY files/model/ DESTINATION ${SVC_DIR} OPTIONAL)
install(DIRECTORY files/voice/ DESTINATION ${SVC_DIR}/voice OPTIONAL)
install(DIRECTORY files/lib/ DESTINATION ${SVC_DIR}/lib OPTIONAL)
install(DIRECTORY files/mosquitto/ DESTINATION ${SVC_DIR}/mosquitto OPTIONAL USE_SOURCE_PERMISSIONS)

# Humidifier USB command path and MCU relay lease.
add_executable(humidifier_test tests/humidifier_test.cpp)
target_include_directories(humidifier_test PRIVATE ../../esp32/nuni_env/include)
target_link_libraries(humidifier_test PRIVATE nuni_core)
target_compile_options(humidifier_test PRIVATE -UNDEBUG)
enable_testing()
foreach(check wired_test usb_serial_test mqtt_keepalive_test humidifier_test)
  add_test(NAME ${check} COMMAND ${check})
endforeach()
install(FILES deploy/65-mic-input-gain.conf DESTINATION ${SVC_DIR}/systemd)
