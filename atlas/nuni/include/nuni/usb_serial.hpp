#pragma once
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <utility>
#include "nuni/sensor_hub.hpp"

namespace nuni {
// An enumerated USB-UART device may stop delivering bytes without POLLHUP.
// Reopen only transports expected to produce periodic traffic. A disconnected
// sensor stays invalid; reopening does not reset SensorHub's source/sequence guards.
class UsbSerialPort {
 public:
  UsbSerialPort(std::string path, std::string channel, double silence_timeout = 0)
      : path_(std::move(path)), channel_(std::move(channel)),
        silence_timeout_(silence_timeout) {}
  ~UsbSerialPort() { close_port(); }
  UsbSerialPort(const UsbSerialPort&) = delete;
  UsbSerialPort& operator=(const UsbSerialPort&) = delete;

  bool is_open() const { return fd_ >= 0; }

  bool write_line(const std::string& line) {
    if(fd_<0 || line.size()>1024 || line.find('\n')!=std::string::npos) return false;
    const std::string frame=line+"\n";
    size_t sent=0;
    for(int attempts=0;sent<frame.size() && attempts<8;++attempts){
      const auto n=::write(fd_,frame.data()+sent,frame.size()-sent);
      if(n>0){sent+=static_cast<size_t>(n);continue;}
      if(n<0 && errno==EINTR)continue;
      if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)){
        pollfd p{fd_,POLLOUT,0};if(::poll(&p,1,25)>0)continue;
      }
      close_port();return false;
    }
    if(sent!=frame.size()){close_port();return false;}
    return true;
  }

  void read(SensorHub& hub, double now) {
    if (fd_ >= 0 && silence_timeout_ > 0 && now - last_bytes_ >= silence_timeout_) {
      std::cerr << "[usb] no bytes for " << silence_timeout_ << "s on "
                << channel_ << "; reopening " << path_ << '\n';
      close_port();
      retry_ = now + 2;
      return;
    }
    if (fd_ < 0) {
      if (now < retry_) return;
      retry_ = now + 2;
      fd_ = open(path_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
      if (fd_ < 0) return;
      termios t{};
      if (tcgetattr(fd_, &t) < 0) { close_port(); return; }
      cfmakeraw(&t);
      cfsetispeed(&t, B115200); cfsetospeed(&t, B115200);
      t.c_cflag |= CLOCAL | CREAD;
      t.c_cflag &= ~(HUPCL | CRTSCTS);
      t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
      if (tcsetattr(fd_, TCSANOW, &t) < 0) { close_port(); return; }
      int bits = TIOCM_DTR | TIOCM_RTS;
      ioctl(fd_, TIOCMBIC, &bits);
      tcflush(fd_, TCIFLUSH);
      pending_.clear();
      last_bytes_ = now;
      received_ = bytes_received_ = false;
      std::cerr << "[usb] opened " << channel_ << " " << path_ << '\n';
    }
    pollfd state{fd_, POLLIN, 0};
    if (::poll(&state, 1, 0) > 0 && (state.revents & (POLLHUP | POLLERR | POLLNVAL))) {
      std::cerr << "[usb] disconnected " << channel_ << '\n';
      close_port(); retry_ = now + 2; return;
    }
    char bytes[1024];
    for (int batch = 0; batch < 8; ++batch) {
      const auto n = ::read(fd_, bytes, sizeof(bytes));
      if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        std::cerr << "[usb] read error " << channel_ << " errno=" << errno << '\n';
        close_port(); retry_ = now + 2; return;
      }
      if (n <= 0) return;
      last_bytes_ = now;
      if (!bytes_received_) {
        bytes_received_ = true;
        std::cerr << "[usb] receiving bytes " << channel_ << '\n';
      }
      for (ssize_t i = 0; i < n; ++i) {
        if (bytes[i] == '\n') {
          if (hub.on_line(channel_, pending_, now) && !received_) {
            received_ = true;
            std::cerr << "[usb] valid frames " << channel_ << '\n';
          }
          pending_.clear();
        } else if (bytes[i] != '\r') {
          pending_ += bytes[i];
        }
        if (pending_.size() > 4096) pending_.clear();
      }
    }
  }

 private:
  void close_port() {
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
    pending_.clear();
  }
  std::string path_, channel_, pending_;
  double silence_timeout_, retry_ = 0, last_bytes_ = 0;
  int fd_ = -1;
  bool received_ = false, bytes_received_ = false;
};
}
