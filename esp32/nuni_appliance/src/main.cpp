// USB-only appliance demonstrator. No mains loads and no wireless networking.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_system.h>

#ifndef NUNI_APPLIANCE_ROLE
#error NUNI_APPLIANCE_ROLE must be set by the PlatformIO environment
#endif
namespace {
constexpr char role[] = NUNI_APPLIANCE_ROLE;
constexpr int led = 2;
char device_id[17], boot[17], line[513];
size_t used = 0;
bool dropping = false, active = false;
uint32_t sequence = 0, last_command = 0, lease = 0, last_report = 0;

void report(const char* reason) {
  JsonDocument d;
  d["v"] = 1; d["kind"] = "state"; d["role"] = role;
  d["device_id"] = device_id; d["boot"] = boot;
  d["seq"] = sequence; d["on"] = active; d["reason"] = reason;
  d["uptime_ms"] = millis();
  Serial.print("NUNIA1 "); serializeJson(d, Serial); Serial.println();
}

void receive() {
  if (strncmp(line, "NUNIA1 ", 7) != 0) return;
  JsonDocument d;
  if (deserializeJson(d, line + 7)) return;
  if (d["v"] != 1 || d["kind"] != "set" || d["role"] != role ||
      d["boot"] != boot || !d["on"].is<bool>() ||
      !d["seq"].is<uint32_t>() || !d["lease_ms"].is<uint32_t>()) return;
  const uint32_t seq = d["seq"], duration = d["lease_ms"];
  if (seq <= sequence || duration < 1000 || duration > 10000) return;
  sequence = seq; active = d["on"].as<bool>();
  last_command = millis(); lease = duration;
  report("accepted");
}
}

void setup() {
  pinMode(led, OUTPUT); digitalWrite(led, LOW);
  snprintf(device_id, sizeof(device_id), "%012llx", ESP.getEfuseMac());
  snprintf(boot, sizeof(boot), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
  Serial.begin(115200);
  report("boot");
}

void loop() {
  // Bound work per loop so noisy serial input cannot starve the lease watchdog.
  for (unsigned count = 0; count < 512 && Serial.available(); ++count) {
    const char c = Serial.read();
    if (c == '\n') {
      if (!dropping) { line[used] = '\0'; receive(); }
      used = 0; dropping = false;
    } else if (c != '\r' && !dropping) {
      if (used < sizeof(line) - 1) line[used++] = c;
      else { used = 0; dropping = true; }
    }
  }
  const uint32_t now = millis();
  if (active && now - last_command >= lease) {
    active = false; report("watchdog_off");
  }
  const bool blink = strcmp(role, "air_purifier") == 0;
  digitalWrite(led, active && (!blink || ((now / 400) % 2 == 0)) ? HIGH : LOW);
  if (now - last_report >= 1000) { report("heartbeat"); last_report = now; }
  delay(2);
}
