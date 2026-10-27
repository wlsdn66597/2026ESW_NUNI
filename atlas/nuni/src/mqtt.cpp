#include "nuni/mqtt.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>

namespace nuni {

namespace {

double mono_now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

constexpr double kRetryS = 2.0;           // 재접속 간격
constexpr double kConnackTimeoutS = 10.0;

void put_u16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v >> 8));
  b.push_back(static_cast<uint8_t>(v & 0xff));
}

void put_str(std::vector<uint8_t>& b, const std::string& s) {
  put_u16(b, static_cast<uint16_t>(s.size()));
  b.insert(b.end(), s.begin(), s.end());
}

// 고정 헤더 + 남은 길이(가변 길이 인코딩) + 본문
std::vector<uint8_t> packet(uint8_t hdr, const std::vector<uint8_t>& body) {
  std::vector<uint8_t> p{hdr};
  size_t len = body.size();
  do {
    uint8_t d = len % 128;
    len /= 128;
    if (len) d |= 0x80;
    p.push_back(d);
  } while (len);
  p.insert(p.end(), body.begin(), body.end());
  return p;
}

}  // namespace

bool topic_matches(const std::string& pattern, const std::string& topic) {
  if (pattern == "#") return true;
  auto split = [](const std::string& s) {
    std::vector<std::string> out;
    size_t a = 0;
    while (true) {
      size_t b = s.find('/', a);
      out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
      if (b == std::string::npos) break;
      a = b + 1;
    }
    return out;
  };
  const auto p = split(pattern), t = split(topic);
  for (size_t i = 0; i < p.size(); ++i) {
    if (p[i] == "#") return true;
    if (i >= t.size()) return false;
    if (p[i] == "+") continue;
    if (p[i] != t[i]) return false;
  }
  return p.size() == t.size();
}

MqttClient::MqttClient(std::string host, int port, std::string client_id, int keepalive_s)
    : host_(std::move(host)), port_(port), client_id_(std::move(client_id)), keepalive_s_(keepalive_s) {}

MqttClient::~MqttClient() {
  if (fd_ >= 0) {
    send_raw({0xE0, 0x00});  // DISCONNECT
    ::close(fd_);
  }
}

void MqttClient::set_credentials(std::string user, std::string password) {
  user_ = std::move(user);
  password_ = std::move(password);
}

bool MqttClient::open_socket() {
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  const std::string port = std::to_string(port_);
  if (getaddrinfo(host_.c_str(), port.c_str(), &hints, &res) != 0 || !res) return false;
  int fd = -1;
  for (addrinfo* ai = res; ai; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    // 연결은 3초 제한(브로커가 없을 때 메인 루프가 오래 멈추지 않게)
    const int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc < 0 && errno == EINPROGRESS) {
      pollfd p{fd, POLLOUT, 0};
      if (::poll(&p, 1, 3000) == 1) {
        int err = 0;
        socklen_t el = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        rc = err == 0 ? 0 : -1;
      } else {
        rc = -1;
      }
    }
    if (rc == 0) {
      fcntl(fd, F_SETFL, fl);  // 쓰기는 블로킹(작은 메시지라 금방 끝난다)
      int one = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      timeval tv{5, 0};
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
      break;
    }
    ::close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) return false;
  {
    std::lock_guard<std::mutex> lk(tx_mu_);
    fd_ = fd;
  }
  rx_.clear();
  ping_pending_ = false;
  return true;
}

void MqttClient::close_socket(const char* why) {
  const bool was = connected_.exchange(false);
  {
    std::lock_guard<std::mutex> lk(tx_mu_);
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }
  if (was) std::cerr << "[mqtt] 연결 끊김(" << why << ") — " << kRetryS << "초 뒤 재접속\n";
  next_attempt_ = mono_now() + kRetryS;
}

bool MqttClient::send_raw(const std::vector<uint8_t>& pkt) {
  std::lock_guard<std::mutex> lk(tx_mu_);
  if (fd_ < 0) return false;
  size_t off = 0;
  while (off < pkt.size()) {
    const ssize_t n = ::send(fd_, pkt.data() + off, pkt.size() - off, MSG_NOSIGNAL);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    off += static_cast<size_t>(n);
  }
  last_tx_ = mono_now();
  return true;
}

void MqttClient::send_subscribe(const std::vector<std::string>& filters) {
  if (filters.empty()) return;
  std::vector<uint8_t> body;
  put_u16(body, next_id_++);
  if (next_id_ == 0) next_id_ = 1;
  for (const auto& f : filters) {
    put_str(body, f);
    body.push_back(0);  // QoS 0
  }
  send_raw(packet(0x82, body));
}

void MqttClient::subscribe(const std::string& filter) {
  {
    std::lock_guard<std::mutex> lk(sub_mu_);
    filters_.push_back(filter);
  }
  if (connected_) send_subscribe({filter});
}

bool MqttClient::publish(const std::string& topic, const std::string& payload, bool retain) {
  if (!connected_) return false;
  std::vector<uint8_t> body;
  put_str(body, topic);
  body.insert(body.end(), payload.begin(), payload.end());
  return send_raw(packet(static_cast<uint8_t>(0x30 | (retain ? 1 : 0)), body));
}

void MqttClient::handle_packet(uint8_t hdr, const uint8_t* b, size_t len) {
  const uint8_t type = hdr >> 4;
  switch (type) {
    case 2: {  // CONNACK
      if (len < 2 || b[1] != 0) {
        std::cerr << "[mqtt] 브로커가 접속을 거절함(코드 " << (len >= 2 ? int(b[1]) : -1) << ")\n";
        close_socket("CONNACK 거절");
        return;
      }
      connected_ = true;
      warned_ = false;
      std::cerr << "[mqtt] 접속됨 " << host_ << ":" << port_ << " (" << client_id_ << ")\n";
      std::vector<std::string> f;
      {
        std::lock_guard<std::mutex> lk(sub_mu_);
        f = filters_;
      }
      send_subscribe(f);  // 재접속 때도 구독 전부 복원
      if (on_connect_) on_connect_();
      break;
    }
    case 3: {  // PUBLISH
      message_retained_ = (hdr & 1) != 0;
      if (len < 2) return;
      const uint8_t qos = (hdr >> 1) & 3;
      const size_t tlen = (size_t(b[0]) << 8) | b[1];
      size_t off = 2 + tlen;
      if (off > len) return;
      std::string topic(reinterpret_cast<const char*>(b + 2), tlen);
      uint16_t pid = 0;
      if (qos > 0) {
        if (off + 2 > len) return;
        pid = static_cast<uint16_t>((b[off] << 8) | b[off + 1]);
        off += 2;
      }
      std::string payload(reinterpret_cast<const char*>(b + off), len - off);
      if (qos == 1) {
        std::vector<uint8_t> ack;
        put_u16(ack, pid);
        send_raw(packet(0x40, ack));  // PUBACK
      } else if (qos == 2) {
        std::vector<uint8_t> rec;
        put_u16(rec, pid);
        send_raw(packet(0x50, rec));  // PUBREC (PUBREL은 아래에서 PUBCOMP로 답한다)
      }
      if (on_message_) {
        try {
          on_message_(topic, payload);
        } catch (const std::exception& e) {
          // 콜백 예외로 수신 루프가 죽지 않게 한다(Python에서 실측으로 겪은 문제)
          std::cerr << "[mqtt] 메시지 처리 중 예외(" << topic << "): " << e.what() << "\n";
        }
      }
      break;
    }
    case 6: {  // PUBREL → PUBCOMP
      if (len >= 2) {
        std::vector<uint8_t> comp{b[0], b[1]};
        send_raw(packet(0x70, comp));
      }
      break;
    }
    case 13:  // PINGRESP
      if (len == 0) ping_pending_ = false;
      break;
    default:  // SUBACK(9), PUBACK(4) 등
      break;
  }
}

void MqttClient::process_input() {
  size_t off = 0;
  while (true) {
    if (rx_.size() - off < 2) break;
    size_t len = 0, mult = 1, i = off + 1;
    bool complete = false;
    for (int k = 0; k < 4 && i < rx_.size(); ++k, ++i) {
      len += (rx_[i] & 0x7f) * mult;
      mult *= 128;
      if (!(rx_[i] & 0x80)) {
        complete = true;
        ++i;
        break;
      }
    }
    if (!complete) {
      if (i - off - 1 >= 4) {  // 남은 길이가 4바이트를 넘으면 잘못된 패킷
        close_socket("잘못된 패킷");
        return;
      }
      break;
    }
    if (rx_.size() - i < len) break;
    const uint8_t hdr = rx_[off];
    handle_packet(hdr, rx_.data() + i, len);
    if (fd_ < 0) return;  // 처리 중 연결이 닫힘
    off = i + len;
  }
  rx_.erase(rx_.begin(), rx_.begin() + static_cast<long>(off));
}

void MqttClient::poll(int timeout_ms) {
  const double now = mono_now();
  if (fd_ < 0) {
    if (now < next_attempt_) {
      ::poll(nullptr, 0, std::min<int>(timeout_ms, static_cast<int>((next_attempt_ - now) * 1000) + 1));
      return;
    }
    if (!open_socket()) {
      if (!warned_) {
        std::cerr << "[mqtt] 브로커 " << host_ << ":" << port_ << " 에 접속할 수 없음 — 계속 재시도\n";
        warned_ = true;
      }
      next_attempt_ = now + kRetryS;
      return;
    }
    // CONNECT
    std::vector<uint8_t> body;
    put_str(body, "MQTT");
    body.push_back(4);  // 프로토콜 3.1.1
    uint8_t flags = 0x02;  // clean session
    if (!user_.empty()) flags |= 0x80;
    if (!password_.empty()) flags |= 0x40;
    body.push_back(flags);
    put_u16(body, static_cast<uint16_t>(keepalive_s_));
    put_str(body, client_id_);
    if (!user_.empty()) put_str(body, user_);
    if (!password_.empty()) put_str(body, password_);
    connect_sent_at_ = mono_now();
    last_rx_ = connect_sent_at_;
    if (!send_raw(packet(0x10, body))) {
      close_socket("CONNECT 전송 실패");
      return;
    }
  }

  pollfd p{fd_, POLLIN, 0};
  const int r = ::poll(&p, 1, timeout_ms);
  if (r > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
    uint8_t buf[4096];
    while (true) {
      const ssize_t n = ::recv(fd_, buf, sizeof(buf), MSG_DONTWAIT);
      if (n > 0) {
        rx_.insert(rx_.end(), buf, buf + n);
        last_rx_ = mono_now();
        if (n < static_cast<ssize_t>(sizeof(buf))) break;
      } else if (n == 0) {
        close_socket("브로커가 연결을 닫음");
        return;
      } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
        close_socket(std::strerror(errno));
        return;
      }
    }
    process_input();
    if (fd_ < 0) return;
  }

  const double t = mono_now();
  if (!connected_ && t - connect_sent_at_ > kConnackTimeoutS) {
    close_socket("CONNACK 시간 초과");
    return;
  }
  if (connected_ && !ping_pending_ &&
      (t - last_tx_ >= keepalive_s_ / 2.0 || t - last_rx_ >= keepalive_s_ / 2.0)) {
    // QoS 0 발행은 응답이 없다. 계속 발행 중이어도 PINGRESP로 연결을 확인한다.
    if (!send_raw({0xC0, 0x00})) {
      close_socket("PINGREQ 전송 실패");
      return;
    }
    ping_sent_at_ = t;
    ping_pending_ = true;
  }
  if (connected_ && ping_pending_ && t - ping_sent_at_ > keepalive_s_)
    close_socket("PINGRESP 시간 초과");
}

}  // namespace nuni
