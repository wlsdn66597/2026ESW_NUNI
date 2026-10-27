// A QoS-0 publisher must request liveness even while continuously sending.
#include "nuni/mqtt.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

bool exact(int fd, unsigned char* data, size_t n) {
  while(n) {
    const auto got=recv(fd,data,n,0);
    if(got<=0)return false;
    data+=got;n-=got;
  }
  return true;
}
int main() {
  const int server=socket(AF_INET,SOCK_STREAM,0);
  sockaddr_in address{};address.sin_family=AF_INET;
  address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  assert(bind(server,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
  socklen_t length=sizeof(address);
  assert(getsockname(server,reinterpret_cast<sockaddr*>(&address),&length)==0);
  assert(listen(server,1)==0);
  std::atomic<bool> reply{true};std::atomic<int> pings{0};
  std::thread broker([&] {
    const int fd=accept(server,nullptr,nullptr);
    assert(fd>=0);
    for(;;) {
      unsigned char header=0,digit=0;
      if(!exact(fd,&header,1))break;
      size_t size=0,multiplier=1;
      do {
        if(!exact(fd,&digit,1)){close(fd);return;}
        size+=(digit&127)*multiplier;multiplier*=128;
        assert(multiplier<=128*128*128*128ULL);
      }while(digit&128);
      std::vector<unsigned char> body(size);
      if(size && !exact(fd,body.data(),size))break;
      if((header>>4)==1) {
        const unsigned char ack[]{0x20,2,0,0};
        assert(send(fd,ack,sizeof(ack),MSG_NOSIGNAL)==sizeof(ack));
      }else if((header>>4)==12) {
        ++pings;
        if(reply) {
          const unsigned char pong[]{0xd0,0};
          assert(send(fd,pong,sizeof(pong),MSG_NOSIGNAL)==sizeof(pong));
        }
      }else if((header>>4)==14)break;
    }
    close(fd);
  });
  {
    nuni::MqttClient client("127.0.0.1",ntohs(address.sin_port),"keepalive-test",2);
    int connections=0;client.set_on_connect([&]{++connections;});
    const auto start=std::chrono::steady_clock::now();
    while(std::chrono::steady_clock::now()-start<std::chrono::seconds(6)) {
      client.poll(10);client.publish("test/value","1");
    }
    assert(client.connected() && connections==1 && pings>=4);
    reply=false;
    const auto loss=std::chrono::steady_clock::now();
    while(client.connected() && std::chrono::steady_clock::now()-loss<std::chrono::seconds(4)) {
      client.poll(10);client.publish("test/value","1");
    }
    assert(!client.connected());
  }
  broker.join();close(server);
  std::cout<<"PASS continuous publisher keepalive and missing PINGRESP timeout\n";
}
