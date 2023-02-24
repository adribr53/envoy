#pragma once

#include "envoy/network/filter.h"

#include "source/common/common/logger.h"

#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

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
    
    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0) {
      ENVOY_LOG(debug, "Failed to create socket");
    }

    memset(&upstream_address_, 0, sizeof(upstream_address_));
    upstream_address_.sin_family = AF_INET;
    upstream_address_.sin_port = htons(upstream_port_);

    if (inet_pton(AF_INET, upstream_ip_.c_str(), &upstream_address_.sin_addr) <= 0) {
      ENVOY_LOG(debug, "Invalid address/Address not supported");
    }

    if (connect(sock_, reinterpret_cast<struct sockaddr*>(&upstream_address_), sizeof(upstream_address_)) < 0) {
      ENVOY_LOG(debug, "Failed to connect to upstream");
    }
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
