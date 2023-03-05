#pragma once

#include "envoy/network/filter.h"
#include "envoy/buffer/buffer.h"
#include "envoy/network/connection.h"

#include "source/common/common/logger.h"
#include "source/common/buffer/buffer_impl.h"

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
namespace Echo {

/**
 * Implementation of a basic echo filter.
 */
class EchoFilter : public Network::ReadFilter, Logger::Loggable<Logger::Id::filter> {
public:
  // Network::ReadFilter
  Network::FilterStatus onData(Buffer::Instance& data, bool end_stream) override;
  Network::FilterStatus onNewConnection() override { return Network::FilterStatus::Continue; }
  void initializeReadFilterCallbacks(Network::ReadFilterCallbacks& callbacks) override {
    read_callbacks_ = &callbacks;
  }
  
  // Destructor
  ~EchoFilter() {
    if (sock_ >= 0) {
      close(sock_);
    }
  }

  // Constructor
  EchoFilter(const std::string& upstream_ip, uint32_t upstream_port)
      : upstream_ip_(upstream_ip), upstream_port_(upstream_port) {

    ENVOY_LOG(debug, "upstream_ip: {}", upstream_ip_);
    ENVOY_LOG(debug, "upstream_port: {}", upstream_port_);
    
    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0) {
      throw EnvoyException("Failed to create socket");
    }

    memset(&upstream_address_, 0, sizeof(upstream_address_));
    upstream_address_.sin_family = AF_INET;
    upstream_address_.sin_port = htons(upstream_port_);

    if (inet_pton(AF_INET, upstream_ip_.c_str(), &upstream_address_.sin_addr) <= 0) {
      throw EnvoyException("Invalid address/Address not supported");
    }

    if (connect(sock_, reinterpret_cast<struct sockaddr*>(&upstream_address_), sizeof(upstream_address_)) < 0) {
      throw EnvoyException("Failed to connect to upstream");
    }

  // Display local socket IP + Port
    struct sockaddr_in source_addr;
    socklen_t source_len = sizeof(source_addr);
    getsockname(sock_, reinterpret_cast<struct sockaddr*>(&source_addr), &source_len);
    ENVOY_LOG(debug, "Socket source IP {}:{}", inet_ntoa(source_addr.sin_addr), std::to_string(ntohs(source_addr.sin_port)));

    // Launch reponses from upstream handler
    std::thread receive_thread(&EchoFilter::receive_messages, this, sock_);
    receive_thread.detach();
  }

  // Handle responses received from upstream: send them back to downstream
  void receive_messages(int socket_fd) {
    const int BUFFER_SIZE = 1024;
    char buffer[BUFFER_SIZE];
    int bytes_received;

    while (true) {
        bytes_received = recv(socket_fd, buffer, BUFFER_SIZE, 0);
        if (bytes_received < 0) {
            ENVOY_LOG(info, "Error receiving message from upstream");
            break;
        } else if (bytes_received == 0) {
            ENVOY_LOG(info, "Upstream closed the connection");
            break;
        }

        std::string message(buffer, bytes_received);
        ENVOY_LOG(info, "Received message from upstream: {}", message);

        Buffer::InstancePtr buffer(new Buffer::OwnedImpl(message));
        auto& connection = read_callbacks_->connection();
        if (connection.state() != Network::Connection::State::Closed) {
          ENVOY_LOG(info, "Sent message to downstream: {}", message);
          connection.write(*buffer, false);
        }
    }
    close(socket_fd);
}

private:
  Network::ReadFilterCallbacks* read_callbacks_{};
  int sock_;
  std::string upstream_ip_;
  uint32_t upstream_port_;
  struct sockaddr_in upstream_address_;
};

} // namespace Echo
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
