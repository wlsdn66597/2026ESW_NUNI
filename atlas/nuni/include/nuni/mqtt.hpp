// 의존성 없는 최소 MQTT 3.1.1 클라이언트(QoS 0 발행·구독).
//
// atlas 기기에는 Paho 라이브러리가 있지만 SDK에 헤더가 없고, 기기마다 라이브러리 유무가 달라
// 배포가 복잡해진다. 우리가 쓰는 기능(QoS 0 발행·구독, keepalive, 재접속)만 직접 구현했다.
// Python bus.py에서 실측으로 얻은 교훈 두 가지를 그대로 지킨다:
//   1) 구독은 접속이 확인(CONNACK)된 뒤에 보낸다
//   2) 재접속하면 구독을 전부 다시 보낸다(안 하면 서비스는 살아 있는데 메시지만 영영 안 온다)
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace nuni {

class MqttClient {
 public:
  using MessageFn = std::function<void(const std::string& topic, const std::string& payload)>;

  MqttClient(std::string host, int port, std::string client_id, int keepalive_s = 60);
  ~MqttClient();
  MqttClient(const MqttClient&) = delete;
  MqttClient& operator=(const MqttClient&) = delete;

  void set_credentials(std::string user, std::string password);
  void set_on_message(MessageFn fn) { on_message_ = std::move(fn); }
  void set_on_connect(std::function<void()> fn) { on_connect_ = std::move(fn); }

  // 구독 필터를 기억해 두고, 접속돼 있으면 바로 보낸다(재접속 때 자동 복원).
  void subscribe(const std::string& filter);

  // QoS 0 발행. 다른 스레드에서 불러도 된다. 접속이 안 돼 있으면 버리고 false.
  bool publish(const std::string& topic, const std::string& payload, bool retain = false);

  // 접속·재접속, 수신 처리, keepalive. 한 스레드(보통 메인 루프)에서만 부른다.
  void poll(int timeout_ms);

  bool connected() const { return connected_; }
  bool message_retained() const { return message_retained_; }
  const std::string& host() const { return host_; }
  int port() const { return port_; }

 private:
  bool open_socket();
  void close_socket(const char* why);
  bool send_raw(const std::vector<uint8_t>& pkt);
  void send_subscribe(const std::vector<std::string>& filters);
  void handle_packet(uint8_t hdr, const uint8_t* body, size_t len);
  void process_input();

  std::string host_;
  int port_;
  std::string client_id_;
  int keepalive_s_;
  std::string user_, password_;
  MessageFn on_message_;
  std::function<void()> on_connect_;

  int fd_ = -1;
  std::atomic<bool> connected_{false};  // CONNACK까지 받은 상태
  std::mutex tx_mu_;                    // fd_ 쓰기·교체 보호
  std::mutex sub_mu_;
  std::vector<std::string> filters_;
  std::vector<uint8_t> rx_;
  std::atomic<double> last_tx_{0};  // send_raw(여러 스레드)가 갱신
  double last_rx_ = 0, next_attempt_ = 0, connect_sent_at_ = 0;
  double ping_sent_at_ = 0;
  bool ping_pending_ = false;
  uint16_t next_id_ = 1;
  bool warned_ = false;
  bool message_retained_ = false;
};

// 토픽 필터 일치 여부(+ 한 단계, # 이하 전부) — bus.py topic_matches와 같은 규칙.
bool topic_matches(const std::string& filter, const std::string& topic);

}  // namespace nuni
