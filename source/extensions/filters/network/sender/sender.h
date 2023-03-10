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
namespace Sender {

/**
 * Implementation of a basic sender filter.
 */
class SenderFilter : public Network::ReadFilter, Logger::Loggable<Logger::Id::filter> {
public:
  // Network::ReadFilter
  Network::FilterStatus onData(Buffer::Instance& data, bool end_stream) override;
  Network::FilterStatus onNewConnection() override { return Network::FilterStatus::Continue; }
  void initializeReadFilterCallbacks(Network::ReadFilterCallbacks& callbacks) override {
    read_callbacks_ = &callbacks;
  }
  
  // Destructor
  ~SenderFilter() {
    if (sock_ >= 0) {
      close(sock_);
    }
    if (sock_rdma_ >= 0) {
      close(sock_rdma_);
    }
    if (sock_distant_rdma_ >= 0) {
      close(sock_distant_rdma_);
    }
  }

  // Constructor
  SenderFilter(const std::string& upstream_ip, uint32_t upstream_port)
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

    sock_rdma_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_rdma_ < 0) {
      throw EnvoyException("Failed to create RDMA socket");
    }

    // bind the socket to an address
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    socklen_t len = sizeof(addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;  // 0 means that the system will assign a free port number
    if (bind(sock_rdma_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw EnvoyException("Binding error");
    }

    // query the socket to find out which port it was bound to
    if (getsockname(sock_rdma_, reinterpret_cast<struct sockaddr*>(&addr), &len) < 0) {
        throw EnvoyException("getsockname failed");
    }

    // Send the port of the second socket to the server
    char msg[32];
    sprintf(msg, "%d", ntohs(addr.sin_port));
    ENVOY_LOG(debug, "msg: {}", msg);
    if (send(sock_, msg, strlen(msg), 0) < 0) {
        throw EnvoyException("send failed");
    }

    if (listen(sock_rdma_, 1) < 0) {
      throw EnvoyException("listen failed");
    }

    ENVOY_LOG(debug, "Waiting for incoming connections...");

    struct sockaddr_in client_addr;
    int addrlen2 = sizeof(client_addr);
    sock_distant_rdma_ = accept(sock_rdma_, reinterpret_cast<struct sockaddr*>(&client_addr), reinterpret_cast<socklen_t*>(&addrlen2));
    if (sock_distant_rdma_ < 0) {
        throw EnvoyException("accept failed");
    }
    ENVOY_LOG(debug, "Connection accepted");

    // Launch TCP reponses from upstream handler
    std::thread upstream_to_downstream_tcp_thread(&SenderFilter::upstream_to_downstream_tcp, this);
    upstream_to_downstream_tcp_thread.detach();

    // Launch RDMA reponses from upstream handler
    std::thread upstream_to_downstream_rdma_thread(&SenderFilter::upstream_to_downstream_rdma, this);
    upstream_to_downstream_rdma_thread.detach();
  }

  // Handle responses received from upstream TCP connection
  void upstream_to_downstream_tcp() {
    const int BUFFER_SIZE = 1024;
    char buffer[BUFFER_SIZE];
    int bytes_received;

    while (true) {
        memset(buffer, '\0', BUFFER_SIZE);
        bytes_received = recv(sock_, buffer, BUFFER_SIZE, 0);
        if (bytes_received < 0) {
            ENVOY_LOG(info, "Error receiving message from TCP upstream");
            break;
        } else if (bytes_received == 0) {
            ENVOY_LOG(info, "TCP Upstream closed the connection");
            break;
        }

        std::string message(buffer, bytes_received);
        ENVOY_LOG(info, "Received message from TCP upstream: {}", message);
    }
    close(sock_);
  }

  // Handle responses received from RDMA upstream: send them back to downstream
  void upstream_to_downstream_rdma() {
    const int BUFFER_SIZE = 1024;
    char buffer[BUFFER_SIZE];
    int bytes_received;

    while (true) {
        memset(buffer, '\0', BUFFER_SIZE);
        bytes_received = recv(sock_distant_rdma_, buffer, BUFFER_SIZE, 0);
        if (bytes_received < 0) {
            ENVOY_LOG(info, "Error receiving message from RDMA upstream");
            break;
        } else if (bytes_received == 0) {
            ENVOY_LOG(info, "RDMA Upstream closed the connection");
            break;
        }

        std::string message(buffer, bytes_received);
        ENVOY_LOG(info, "Received message from RDMA upstream: {}", message);

        Buffer::InstancePtr buffer(new Buffer::OwnedImpl(message));
        auto& connection = read_callbacks_->connection();
        if (connection.state() != Network::Connection::State::Closed) {
          // ENVOY_LOG(info, "Sent message to downstream: {}", message);
          connection.write(*buffer, false);
        }
    }
    close(sock_distant_rdma_);
    close(sock_rdma_);
  }

private:
  Network::ReadFilterCallbacks* read_callbacks_{};
  int sock_;
  std::string upstream_ip_;
  uint32_t upstream_port_;
  struct sockaddr_in upstream_address_;
  int sock_rdma_;
  int sock_distant_rdma_;
};

} // namespace Sender
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
