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
namespace Sender {

/**
 * Implementation of a basic sender filter.
 */
class SenderFilter : public Network::ReadFilter, public Network::ConnectionCallbacks, Logger::Loggable<Logger::Id::filter> {
public:
  // Network::ReadFilter
  Network::FilterStatus onData(Buffer::Instance& data, bool end_stream) override;
  Network::FilterStatus onNewConnection() override;
  void initializeReadFilterCallbacks(Network::ReadFilterCallbacks& callbacks) override {
    read_callbacks_ = &callbacks;
    read_callbacks_->connection().addConnectionCallbacks(*this);
  }

  void onEvent(Network::ConnectionEvent event) override {
    if (event == Network::ConnectionEvent::RemoteClose ||
        event == Network::ConnectionEvent::LocalClose) {
      // Connection was closed, perform any necessary cleanup
      stop_flag_from_downstream_ = 1; // Flag to indicate that downstream closed the connection and threads should stop
      ENVOY_LOG(debug, "CONNECTION CLOSED");
    } 
  }

  void onAboveWriteBufferHighWatermark() override {
  }

  void onBelowWriteBufferLowWatermark() override {
  }
  
  // Destructor
  ~SenderFilter() {
    // close(sock_);
    // close(sock_rdma_);
    // close(sock_distant_rdma_);
    // stop_flag_from_downstream_ = 1;
    ENVOY_LOG(debug, "DESTRUCTOR");
  }

  // Constructor
  SenderFilter(const std::string& upstream_ip, uint32_t upstream_port)
      : upstream_ip_(upstream_ip), upstream_port_(upstream_port) {

    ENVOY_LOG(debug, "upstream_ip: {}", upstream_ip_);
    ENVOY_LOG(debug, "upstream_port: {}", upstream_port_);
  }

  // Handle responses received from upstream TCP connection
  void upstream_to_downstream_tcp() {
    const int BUFFER_SIZE = 1024;
    char buffer[BUFFER_SIZE];
    int bytes_received;

    struct pollfd poll_fds[1];
    poll_fds[0].fd = sock_;
    poll_fds[0].events = POLLIN;

    while (true) {
      int ret = poll(poll_fds, 1, 0);

      if (stop_flag_from_downstream_ == 1) {
        ENVOY_LOG(info, "Close upstream_to_downstream_tcp thread due to downstream stop");
        // Close connection with upstream listener
        close(sock_);
        return;
      }

      if (ret < 0) {
        ENVOY_LOG(error, "Poll error");
        return;
      }

      else if (ret == 0) {
         if (stop_flag_from_downstream_ == 1) {
          ENVOY_LOG(info, "Close upstream_to_downstream_tcp thread due to downstream stop");
          // Close connection with upstream listener
          close(sock_);
          return;
        }
      }

      else if (poll_fds[0].revents & POLLIN) {
        memset(buffer, '\0', BUFFER_SIZE);
        bytes_received = recv(sock_, buffer, BUFFER_SIZE, 0);
        if (bytes_received < 0) {
            ENVOY_LOG(info, "Error receiving message from TCP upstream");
            stop_flag_from_upstream_ = 1;
            ENVOY_LOG(info, "Close upstream_to_downstream_tcp thread due to upstream stop");
            close(sock_);
            return;
        } 
        else if (bytes_received == 0) {
            ENVOY_LOG(info, "TCP Upstream closed the connection");
            stop_flag_from_upstream_ = 1;
            ENVOY_LOG(info, "Close upstream_to_downstream_tcp thread due to upstream stop");
            close(sock_);
            return;
        }

        std::string message(buffer, bytes_received);
        ENVOY_LOG(info, "Received message from TCP upstream: {}", message);
      }
    }
  }

  // Handle responses received from RDMA upstream: send them back to downstream (use polling)
  void upstream_to_downstream_rdma() {
    const int BUFFER_SIZE = 1024;
    char buffer[BUFFER_SIZE];
    int bytes_received;

    struct pollfd poll_fds[1];
    poll_fds[0].fd = sock_distant_rdma_;
    poll_fds[0].events = POLLIN;

    while (true) {
      int ret = poll(poll_fds, 1, 0);

      if (stop_flag_from_downstream_ == 1) {
        ENVOY_LOG(info, "Close upstream_to_downstream_rdma thread due to downstream stop");
        close(sock_rdma_);
        close(sock_distant_rdma_);
        return;
      }
      if (stop_flag_from_upstream_ == 1) {
        ENVOY_LOG(info, "Close upstream_to_downstream_rdma thread due to upstream stop");
        close(sock_rdma_);
        close(sock_distant_rdma_);
        read_callbacks_->connection().close(Network::ConnectionCloseType::Abort);
        return;
      }

      if (ret < 0) {
        ENVOY_LOG(error, "Poll error");
        return;
      }

      else if (ret == 0) {
        // ENVOY_LOG(info, "Timeout expired");
        if (stop_flag_from_downstream_ == 1) {
          ENVOY_LOG(info, "Close upstream_to_downstream_rdma thread due to downstream stop");
          close(sock_rdma_);
          close(sock_distant_rdma_);
          return;
        }
        if (stop_flag_from_upstream_ == 1) {
          ENVOY_LOG(info, "Close upstream_to_downstream_rdma thread due to upstream stop");
          close(sock_rdma_);
          close(sock_distant_rdma_);
          read_callbacks_->connection().close(Network::ConnectionCloseType::Abort);
          return;
        }
      }

      else if (poll_fds[0].revents & POLLIN) {
        memset(buffer, '\0', BUFFER_SIZE);
        bytes_received = recv(sock_distant_rdma_, buffer, BUFFER_SIZE, 0);
        if (bytes_received < 0) {
            ENVOY_LOG(info, "Error receiving message from RDMA upstream");
            continue;
        } 
        else if (bytes_received == 0) {
            ENVOY_LOG(info, "RDMA Upstream closed the connection");
            continue;
        }

        std::string message(buffer, bytes_received);
        ENVOY_LOG(info, "Received message from RDMA upstream: {}", message);

        if (stop_flag_from_downstream_ == 1) {
          ENVOY_LOG(info, "2.Close upstream_to_downstream_rdma thread due to downstream stop");
          close(sock_rdma_);
          close(sock_distant_rdma_);
          return;
        }

        if (stop_flag_from_upstream_ == 1) {
          ENVOY_LOG(info, "2.Close upstream_to_downstream_rdma thread due to upstream stop");
          close(sock_rdma_);
          close(sock_distant_rdma_);
          read_callbacks_->connection().close(Network::ConnectionCloseType::Abort);
          return;
        }
        
        Buffer::InstancePtr buffer2(new Buffer::OwnedImpl(message));
        ENVOY_LOG(error, "BEFORE TEST");
        if (read_callbacks_ == nullptr) {
          ENVOY_LOG(error, "read_callbacks_ NULL");
        } 
        else {
          ENVOY_LOG(error, "read_callbacks_ NOT NULL");
        }
        // Network::Connection& connection = read_callbacks_->connection();
        ENVOY_LOG(error, "GONNA WRITE TO CLIENT");
        // if (connection.state() == Network::Connection::State::Open) {
        //   // ENVOY_LOG(info, "Sent message to downstream: {}", message);
        //   ENVOY_LOG(info, "buffer2: {}", buffer2.get()->toString());
        //   connection.write(*buffer2, false); // connection object should be available or causes segmentation fault
        //   ENVOY_LOG(error, "WRITTEN TO CLIENT");
        // }
        // else {
        //   ENVOY_LOG(error, "Client closed the connection");
        // }
        read_callbacks_->connection().write(*buffer2, false);
      }
    }
  }

private:
  Network::ReadFilterCallbacks* read_callbacks_{};
  int sock_;
  std::string upstream_ip_;
  uint32_t upstream_port_;
  struct sockaddr_in upstream_address_;
  int sock_rdma_;
  int sock_distant_rdma_;
  int stop_flag_from_upstream_ = 0;
  int stop_flag_from_downstream_ = 0;
};

} // namespace Sender
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
