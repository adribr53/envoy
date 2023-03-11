#include "source/extensions/filters/network/sender/sender.h"

#include "envoy/buffer/buffer.h"
#include "envoy/network/connection.h"

#include "source/common/common/assert.h"

#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <thread>

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace Sender {

Network::FilterStatus SenderFilter::onNewConnection() {
  sock_ = socket(AF_INET, SOCK_STREAM, 0);
  if (sock_ < 0) {
    ENVOY_LOG(error, "Failed to create socket");
    read_callbacks_->connection().close(Network::ConnectionCloseType::Abort);
    return Network::FilterStatus::StopIteration; 
  }

  memset(&upstream_address_, 0, sizeof(upstream_address_));
  upstream_address_.sin_family = AF_INET;
  upstream_address_.sin_port = htons(upstream_port_);

  if (inet_pton(AF_INET, upstream_ip_.c_str(), &upstream_address_.sin_addr) <= 0) {
    ENVOY_LOG(error, "Invalid address/Address not supported");
    read_callbacks_->connection().close(Network::ConnectionCloseType::Abort);
    return Network::FilterStatus::StopIteration; 
  }

  if (connect(sock_, reinterpret_cast<struct sockaddr*>(&upstream_address_), sizeof(upstream_address_)) < 0) {
    ENVOY_LOG(error, "Failed to connect to upstream");
    read_callbacks_->connection().close(Network::ConnectionCloseType::Abort);
    return Network::FilterStatus::StopIteration; 
  }

  // Display local socket IP + Port
  struct sockaddr_in source_addr;
  socklen_t source_len = sizeof(source_addr);
  getsockname(sock_, reinterpret_cast<struct sockaddr*>(&source_addr), &source_len);
  ENVOY_LOG(debug, "Socket source IP {}:{}", inet_ntoa(source_addr.sin_addr), std::to_string(ntohs(source_addr.sin_port)));

  sock_rdma_ = socket(AF_INET, SOCK_STREAM, 0);
  if (sock_rdma_ < 0) {
    ENVOY_LOG(error, "Failed to create RDMA socket");
    read_callbacks_->connection().close(Network::ConnectionCloseType::Abort);
    return Network::FilterStatus::StopIteration; 
  }

  // bind the socket to an address
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  socklen_t len = sizeof(addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = 0;  // 0 means that the system will assign a free port number
  if (bind(sock_rdma_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    ENVOY_LOG(error, "Binding error");
    read_callbacks_->connection().close(Network::ConnectionCloseType::Abort);
    return Network::FilterStatus::StopIteration; 
  }

  // query the socket to find out which port it was bound to
  if (getsockname(sock_rdma_, reinterpret_cast<struct sockaddr*>(&addr), &len) < 0) {
    ENVOY_LOG(error, "getsockname failed");
    read_callbacks_->connection().close(Network::ConnectionCloseType::Abort);
    return Network::FilterStatus::StopIteration; 
  }

  // Send the port of the RDMA socket to the server
  char msg[32];
  sprintf(msg, "%d", ntohs(addr.sin_port));
  ENVOY_LOG(debug, "port: {}", msg);
  if (send(sock_, msg, strlen(msg), 0) < 0) {
    ENVOY_LOG(error, "Send port to remote listener failed");
    read_callbacks_->connection().close(Network::ConnectionCloseType::Abort);
    return Network::FilterStatus::StopIteration; 
  }

  // Wait to receive an ACK from upstream listener (ACK confirms that upstream listener could connect to the server)
  char ack[6];
  memset(ack, '\0', 6);
  recv(sock_, ack, 6, 0); // Should not block on recv in case of failure of upstream listener
  ENVOY_LOG(debug, "MESSAGE: {}", ack);
  ENVOY_LOG(debug, "{}", strcmp(ack, "ERROR"));

  if (strcmp(ack, "ERROR") == 0) {
    ENVOY_LOG(error, "Upstream server unreachable");
    read_callbacks_->connection().close(Network::ConnectionCloseType::Abort);
    close(sock_);
    return Network::FilterStatus::StopIteration;
  }

  ENVOY_LOG(debug, "ACK OK");

  // Send back an ACK to upstream listener so that it can connect to RDMA socket
  const char *ack2 = "ACK2";
  if (send(sock_, ack2, strlen(ack2), 0) < 0) {
    ENVOY_LOG(error, "Send ack2 to upstream listener failed");
    read_callbacks_->connection().close(Network::ConnectionCloseType::Abort);
    return Network::FilterStatus::StopIteration; 
  }

  // RDMA connection can be established now
  if (listen(sock_rdma_, 1) < 0) {
    ENVOY_LOG(error, "Listen failed");
    read_callbacks_->connection().close(Network::ConnectionCloseType::Abort);
    return Network::FilterStatus::StopIteration; 
  }

  ENVOY_LOG(debug, "Waiting for incoming connections...");

  struct sockaddr_in client_addr;
  int addrlen2 = sizeof(client_addr);
  sock_distant_rdma_ = accept(sock_rdma_, reinterpret_cast<struct sockaddr*>(&client_addr), reinterpret_cast<socklen_t*>(&addrlen2));
  if (sock_distant_rdma_ < 0) {
    ENVOY_LOG(error, "Accept failed");
    read_callbacks_->connection().close(Network::ConnectionCloseType::Abort);
    return Network::FilterStatus::StopIteration; 
  }

  ENVOY_LOG(debug, "RDMA Connection from remote listener accepted");

  // Launch TCP reponses from upstream handler
  std::thread upstream_to_downstream_tcp_thread(&SenderFilter::upstream_to_downstream_tcp, this);
  upstream_to_downstream_tcp_thread.detach();

  // Launch RDMA reponses from upstream handler
  std::thread upstream_to_downstream_rdma_thread(&SenderFilter::upstream_to_downstream_rdma, this);
  upstream_to_downstream_rdma_thread.detach();

  return Network::FilterStatus::Continue;
}


Network::FilterStatus SenderFilter::onData(Buffer::Instance& data, bool end_stream) {
  ENVOY_CONN_LOG(trace, "sender: got {} bytes", read_callbacks_->connection(), data.length());

  std::string output = data.toString();
  ENVOY_LOG(debug, output);
  ENVOY_LOG(debug, end_stream);

  // // Extract the flag field from the buffer
  // uint8_t flag;
  // data.copyOut(0, sizeof(flag), &flag);
  // ENVOY_LOG(debug, "flag: {}", flag);
  
  // // Extract the str field from the buffer
  // char str[10];
  // memset(str, '\0', sizeof(str));
  // data.copyOut(sizeof(flag), sizeof(str), str);
  // ENVOY_LOG(debug, "str: {}", str);
  
  // // Extract the num field from the buffer
  // int32_t num;
  // data.copyOut(sizeof(flag) + sizeof(str), sizeof(num), &num);
  // ENVOY_LOG(debug, "num: {}", ntohl(num));
  
  // // Extract the payload field from the buffer
  // char payload[256];
  // memset(payload, '\0', sizeof(payload));
  // data.copyOut(sizeof(flag) + sizeof(str) + sizeof(num), sizeof(payload), payload);
  // ENVOY_LOG(debug, "payload: {}", payload);
  
  // Print downstream IP + port
  Network::Connection& connection = read_callbacks_->connection();
  const auto& stream_info = connection.streamInfo();
  Network::Address::InstanceConstSharedPtr remote_address = stream_info.downstreamAddressProvider().remoteAddress();
  std::string source_ip = remote_address->ip()->addressAsString();
  uint32_t source_port = remote_address->ip()->port();
  ENVOY_LOG(debug, "Received {} bytes from {}:{}", data.length(), source_ip, source_port);

  // Forward requests from downstream to upstream using RDMA
  int message_length = data.length();
  int bytes_sent = send(sock_distant_rdma_, data.toString().c_str(), message_length, 0);
  if (bytes_sent != message_length) {
    ENVOY_LOG(error, "Failed to send RDMA message to upstream listener");
    data.drain(data.length());
    read_callbacks_->connection().close(Network::ConnectionCloseType::Abort);
    return Network::FilterStatus::StopIteration; 
  }

  data.drain(data.length());
  return Network::FilterStatus::StopIteration;
} 

} // namespace Sender
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
