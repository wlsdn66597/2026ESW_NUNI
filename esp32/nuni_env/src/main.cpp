// USB-only: SCD41 + VL53L1X -> Pi 4 nunisensors.
// SDA=GPIO21, SCL=GPIO22, VCC=3V3, GND; addresses 0x62 / 0x29.
// NUNI1 JSON lines at 115200 baud: env (~5 s), tof (~1 s).
// Pi 4 evaluates a sustained beam intrusion; this is not a posture classifier.
#include <Arduino.h>
#include <esp_system.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <SensirionI2CScd4x.h>
#include <VL53L1X.h>
#include <algorithm>
#include "relay_lease.h"

#ifndef NUNI_RELAY_PIN
#define NUNI_RELAY_PIN 23
#endif
#ifndef NUNI_RELAY_ACTIVE_LOW
#define NUNI_RELAY_ACTIVE_LOW 1
#endif
RelayLease relay;
char relay_line[513],relay_command_id[101]="";
size_t relay_used=0;
bool relay_dropping=false;
uint32_t relay_report_at=0;

static const char* TOPIC_ENV = "env";
static const char* TOPIC_TOF = "tof";
char boot_id[24];
uint32_t serial_seq=0;
static const int NEAR_MM = 600;          // 이보다 가까우면 near=true (센서 앞에 사람/물체)
static const int TOF_SAMPLES = 5;        // 1초에 5번 재서 중앙값
static const uint32_t TOF_PERIOD_MS = 1000;
static const uint32_t SCD_POLL_MS = 1000;  // 데이터 준비 여부 확인 주기(측정 자체는 5초)

SensirionI2CScd4x scd4x;
VL53L1X tof;
bool scd_ok = false, tof_ok = false;
uint32_t tof_last_init = 0;
int tof_timeout_batches = 0;

static uint8_t tof_ack() {
  Wire.beginTransmission(0x29);
  return Wire.endTransmission();
}

static double now_ts() { return millis()/1000.0; } // device uptime, never NTP
static void publish_json(const char* kind, JsonDocument& doc) {
  JsonDocument frame;
  frame["v"]=1; frame["device"]="env"; frame["boot_id"]=boot_id;
  frame["seq"]=++serial_seq; frame["uptime_ms"]=millis(); frame["kind"]=kind;
  frame["data"].set(doc.as<JsonVariant>());
  Serial.print("NUNI1 "); serializeJson(frame,Serial); Serial.println();
}

static void relay_output(){digitalWrite(NUNI_RELAY_PIN,relay.on()?(NUNI_RELAY_ACTIVE_LOW?LOW:HIGH):(NUNI_RELAY_ACTIVE_LOW?HIGH:LOW));}
static void relay_report(const char* reason){
  JsonDocument data;data["role"]="humidifier";data["on"]=relay.on();
  data["relay_seq"]=relay.sequence();data["command_id"]=relay_command_id;data["reason"]=reason;
  publish_json("relay",data);relay_report_at=millis();
}
static void relay_receive(){
  if(strncmp(relay_line,"NUNIH1 ",7)!=0)return;
  JsonDocument d;if(deserializeJson(d,relay_line+7))return;
  if(d["v"]!=1 || d["role"]!="humidifier" || d["boot_id"]!=boot_id ||
     !d["seq"].is<uint32_t>() || !d["on"].is<bool>() || !d["lease_ms"].is<uint32_t>() ||
     !d["command_id"].is<const char*>())return;
  const char* id=d["command_id"];if(!id || strlen(id)==0 || strlen(id)>100)return;
  if(!relay.accept(d["seq"],d["on"],d["lease_ms"],millis()))return;
  strlcpy(relay_command_id,id,sizeof(relay_command_id));relay_output();relay_report("accepted");
}
static void relay_poll(){
  for(unsigned count=0;count<512 && Serial.available();++count){
    const char ch=Serial.read();
    if(ch=='\n'){
      if(!relay_dropping){relay_line[relay_used]='\0';relay_receive();}
      relay_used=0;relay_dropping=false;
    }else if(ch!='\r' && !relay_dropping){
      if(relay_used<sizeof(relay_line)-1)relay_line[relay_used++]=ch;
      else{relay_used=0;relay_dropping=true;}
    }
  }
  if(relay.tick(millis())){relay_output();relay_report("lease_expired");}
  if(millis()-relay_report_at>=1000)relay_report("heartbeat");
}

static void setup_scd41() {
  scd4x.begin(Wire);
  scd4x.stopPeriodicMeasurement();  // 이전 부팅에서 측정 중이었으면 멈춰야 설정 명령이 먹는다
  delay(500);
  uint16_t err = scd4x.startPeriodicMeasurement();
  scd_ok = (err == 0);
  Serial.printf("[scd41] %s\n", scd_ok ? "OK" : "응답 없음 (배선·주소 0x62 확인)");
}

static void setup_tof() {
  tof_last_init = millis();
  tof_ok = false;
  const uint8_t ack = tof_ack();
  if (ack != 0) {
    Serial.printf("[tof] I2C 0x29 ACK 실패=%u; 10초 후 재시도\n", ack);
    return;
  }
  // init() does not reset all library calibration state. A fresh object is
  // needed when recovering from a stopped or disconnected sensor.
  tof = VL53L1X();
  tof.setBus(&Wire);
  tof.setTimeout(500);
  const uint16_t model_id = tof.readReg16Bit(VL53L1X::IDENTIFICATION__MODEL_ID);
  Serial.printf("[tof] ACK=0 model_id=0x%04X (expected 0xEACC), i2c=%u\n", model_id, tof.last_status);
  tof_ok = tof.init();
  if (tof_ok) {
    // 먼저 기본 수광 영역에서 거리 측정을 검증한다. 난간용 좁은 ROI는
    // 실제 설치 각도와 거리 검증 뒤 적용한다(8x8 시험에서 무효값 다수).
    const bool mode_ok = tof.setDistanceMode(VL53L1X::Short);
    tof.setROISize(16, 16);
    const bool budget_ok = tof.setMeasurementTimingBudget(50000);
    tof_ok = mode_ok && budget_ok && tof.last_status == 0;
    if (tof_ok) tof.startContinuous(100);
    Serial.printf("[tof] mode=%d budget=%d ROI=16x16 period=100ms i2c=%u\n", mode_ok, budget_ok, tof.last_status);
  }
  tof_timeout_batches = 0;
  Serial.printf("[tof] %s\n", tof_ok ? "OK" : "응답 없음 (배선·주소 0x29 확인)");
}

static void poll_scd41() {
  static uint32_t last = 0;
  if (!scd_ok || millis() - last < SCD_POLL_MS) return;
  last = millis();
  bool ready = false;
  if (scd4x.getDataReadyFlag(ready) || !ready) return;
  uint16_t co2 = 0;
  float temp = 0, hum = 0;
  if (scd4x.readMeasurement(co2, temp, hum) || co2 == 0) return;  // co2==0 은 무효 샘플
  JsonDocument doc;
  doc["valid"] = true;
  doc["temp"] = round(temp * 10) / 10.0;
  doc["humidity"] = round(hum * 10) / 10.0;
  doc["co2"] = co2;
  doc["ts"] = now_ts();
  publish_json(TOPIC_ENV, doc);
}

static void poll_tof() {
  static uint32_t last = 0;
  if (!tof_ok) {
    if (millis() - tof_last_init >= 10000) setup_tof();
    return;
  }
  if (millis() - last < TOF_PERIOD_MS) return;
  last = millis();
  int vals[TOF_SAMPLES];
  int n = 0;
  int timeouts = 0;
  int last_status = -1;
  for (int i = 0; i < TOF_SAMPLES; i++) {
    uint16_t mm = tof.read();  // 연속 모드: 다음 측정(약 50ms)까지 기다림
    relay_poll();
    const bool timed_out = tof.timeoutOccurred();
    if (timed_out) ++timeouts;
    last_status = static_cast<int>(tof.ranging_data.range_status);
    if (!timed_out && mm > 0 && tof.ranging_data.range_status == VL53L1X::RangeValid)
      vals[n++] = mm;
  }
  JsonDocument doc;
  bool valid = n >= (TOF_SAMPLES + 1) / 2;  // 과반이 유효할 때만 믿는다
  int dist = -1;
  if (valid) {
    std::sort(vals, vals + n);
    dist = vals[n / 2];
  }
  doc["distance_mm"] = dist;
  doc["valid"] = valid;
  doc["near"] = valid && dist < NEAR_MM;
  doc["range_status"] = last_status;
  doc["timeouts"] = timeouts;
  doc["samples_valid"] = n;
  const uint8_t ack = tof_ack();
  doc["i2c_ack"] = ack;
  doc["ts"] = now_ts();
  publish_json(TOPIC_TOF, doc);
  if (timeouts == TOF_SAMPLES || ack != 0) ++tof_timeout_batches;
  else tof_timeout_batches = 0;
  if (tof_timeout_batches >= 3) {
    Serial.printf("[tof] 3개 묶음 연속 통신/측정 실패, i2c_ack=%u; 재초기화 예약\n", ack);
    tof_ok = false;
    tof_last_init = millis();
  }
}

void setup() {
  relay_output();pinMode(NUNI_RELAY_PIN,OUTPUT);relay_output();
  Serial.begin(115200);
  delay(200);
  snprintf(boot_id,sizeof(boot_id),"%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random());
  Serial.println("[nuni-env] USB-only v1; SDA21 SCL22");
  relay_report("boot");
  Wire.begin(21,22); Wire.setClock(100000);
  setup_scd41(); setup_tof();
}
void loop() {
  static uint32_t scd_retry=0;
  if(!scd_ok && millis()-scd_retry>=15000) { scd_retry=millis(); setup_scd41(); }
  relay_poll();poll_scd41();poll_tof();relay_poll();delay(10);
}
