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
#include <sys/poll.h>
#include <unistd.h>
#include <cstring>
#include <thread>

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace Receiver {

/**
 * Implementation of a basic receiver filter.
 */
class ReceiverFilter : public Network::ReadFilter, Logger::Loggable<Logger::Id::filter> {
public:
  // Network::ReadFilter
  Network::FilterStatus onData(Buffer::Instance& data, bool end_stream) override;
  Network::FilterStatus onNewConnection() override { return Network::FilterStatus::Continue; }
  void initializeReadFilterCallbacks(Network::ReadFilterCallbacks& callbacks) override {
    read_callbacks_ = &callbacks;
  }
  
  // Destructor
  ~ReceiverFilter() {
    // close(rdma_sock_);
    // close(upstream_sock_);
    stop_flag_from_downstream_ = 1;
    ENVOY_LOG(debug, "DESTRUCTOR");
  }

  // Constructor
  ReceiverFilter(const std::string& upstream_ip, uint32_t upstream_port)
      : upstream_ip_(upstream_ip), upstream_port_(upstream_port) {

    ENVOY_LOG(debug, "upstream_ip: {}", upstream_ip_);
    ENVOY_LOG(debug, "upstream_port: {}", upstream_port_);
  }

  // Handle requests received from downstream: forward them to upstream (RDMA)
  void downstream_to_upstream() {
    const int BUFFER_SIZE = 1024;
    char buffer[BUFFER_SIZE];
    int bytes_received;

    struct pollfd poll_fds[1];
    poll_fds[0].fd = rdma_sock_;
    poll_fds[0].events = POLLIN;

    while (true) {
      int ret = poll(poll_fds, 1, 0);

      if (stop_flag_from_upstream_ == 1) {
        ENVOY_LOG(info, "Close downstream_to_upstream_rdma thread due to upstream stop");
        read_callbacks_->connection().close(Network::ConnectionCloseType::Abort);
        return;
      }
      else if (stop_flag_from_downstream_ == 1) {
        ENVOY_LOG(info, "Close downstream_to_upstream_rdma thread due to downstream stop");
        return;
      }

      if (ret < 0) {
        ENVOY_LOG(error, "Poll error");
        return;
      }

      else if (ret == 0) {
        // ENVOY_LOG(info, "Timeout expired");
        if (stop_flag_from_upstream_ == 1) {
          ENVOY_LOG(info, "Close downstream_to_upstream_rdma thread due to upstream stop");
          read_callbacks_->connection().close(Network::ConnectionCloseType::Abort);
          return;
        }
        else if (stop_flag_from_downstream_ == 1) {
          ENVOY_LOG(info, "Close downstream_to_upstream_rdma thread due to downstream stop");
          return;
        }
      }

      else if (poll_fds[0].revents & POLLIN) {
        memset(buffer, '\0', BUFFER_SIZE);
        bytes_received = recv(rdma_sock_, buffer, BUFFER_SIZE, 0);
        if (bytes_received < 0) {
            ENVOY_LOG(error, "Error receiving message from RDMA downstream");
            continue;
        } 
        else if (bytes_received == 0) {
            ENVOY_LOG(info, "RDMA Downstream closed the connection");
            continue;
        }

        std::string message(buffer, bytes_received);
        ENVOY_LOG(info, "Received message from RDMA downstream: {}", message);
        int bytes_sent = send(upstream_sock_, buffer, bytes_received, 0); // upstream_sock_ should not be closed
        if (bytes_sent != bytes_received) {
            ENVOY_LOG(error, "Failed to send message to TCP upstream");
          }
      }
    }
}

  // Handle responses received from upstream: forward them to downstream (TCP)
  void upstream_to_downstream() {
    const int BUFFER_SIZE = 1024;
    char buffer[BUFFER_SIZE];
    int bytes_received;

   struct pollfd poll_fds[1];
    poll_fds[0].fd = upstream_sock_;
    poll_fds[0].events = POLLIN;

    while (true) {
      int ret = poll(poll_fds, 1, 0);

      if (stop_flag_from_downstream_ == 1) {
        ENVOY_LOG(info, "Close upstream_to_downstream_tcp thread due to downstream stop");
        close(upstream_sock_);
        return;
      }

      if (ret < 0) {
        ENVOY_LOG(error, "Poll error");
        return;
      }

      else if (ret == 0) {
        // ENVOY_LOG(info, "Timeout expired");
        if (stop_flag_from_downstream_ == 1) {
          ENVOY_LOG(info, "Close upstream_to_downstream_tcp thread due to downstream stop");
          close(upstream_sock_);
          return;
        }
      }

      else if (poll_fds[0].revents & POLLIN) {
        memset(buffer, '\0', BUFFER_SIZE);
        bytes_received = recv(upstream_sock_, buffer, BUFFER_SIZE, 0);
        if (bytes_received < 0) {
            ENVOY_LOG(error, "Error receiving message from TCP upstream: {}, {}", bytes_received, errno);
            ENVOY_LOG(info, "Close upstream_to_downstream_tcp thread due to upstream stop");
            stop_flag_from_upstream_ = 1;
            return;
        } 
        else if (bytes_received == 0) {
            ENVOY_LOG(info, "TCP upstream closed the connection");
            ENVOY_LOG(info, "Close upstream_to_downstream_tcp thread due to upstream stop");
            stop_flag_from_upstream_ = 1;
            return;
        }

        std::string message(buffer, bytes_received);
        ENVOY_LOG(info, "Received message from TCP upstream: {}", message);

        int bytes_sent = send(rdma_sock_, buffer, bytes_received, 0);
        if (bytes_sent != bytes_received) {
            ENVOY_LOG(error, "Failed to send message to RDMA downstream");
        }
      }
    }
}

private:
  Network::ReadFilterCallbacks* read_callbacks_{};
  std::string upstream_ip_;
  uint32_t upstream_port_;
  int rdma_sock_;
  int flag_ = 0;
  int upstream_sock_;
  struct sockaddr_in upstream_address_;
  uint32_t downstream_port_;
  int stop_flag_from_upstream_ = 0;
  int stop_flag_from_downstream_ = 0;
};

} // namespace Receiver
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
