#include "source/extensions/filters/network/receiver/receiver.h"

#include "envoy/buffer/buffer.h"
#include "envoy/network/connection.h"

#include "source/common/common/assert.h"

#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace Receiver {

Network::FilterStatus ReceiverFilter::onData(Buffer::Instance& data, bool end_stream) {
  if (flag_ == 0) {
    ENVOY_CONN_LOG(trace, "receiver: got {} bytes", read_callbacks_->connection(), data.length());

    std::string output = data.toString();
    ENVOY_LOG(debug, output);
    ENVOY_LOG(debug, end_stream);
    downstream_port_ = std::stoul(data.toString());

    // Connect to upstream (TCP server)
    upstream_sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (upstream_sock_ < 0) {
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

    if (connect(upstream_sock_, reinterpret_cast<struct sockaddr*>(&upstream_address_), sizeof(upstream_address_)) < 0) {
      // If connection to upstream server fails
      ENVOY_LOG(error, "TCP Failed to connect to upstream");
      Buffer::InstancePtr buffer(new Buffer::OwnedImpl("ERROR"));
      auto& connection = read_callbacks_->connection();
      if (connection.state() != Network::Connection::State::Closed) {
        connection.write(*buffer, false);
      }
      // read_callbacks_->connection().close(Network::ConnectionCloseType::Abort);
      return Network::FilterStatus::StopIteration;
    }
    // If connection to server is successful
    ENVOY_LOG(debug, "CONNECTED TO UPSTREAM SERVER");
    Buffer::InstancePtr buffer(new Buffer::OwnedImpl("ACK"));
    auto& connection = read_callbacks_->connection();
    if (connection.state() != Network::Connection::State::Closed) {
      connection.write(*buffer, false);
    }
  }

  if (flag_ == 1) {
    // Receive second ACK to indicate that downstream listener now listens to RDMA socket
    std::string ack2 = data.toString();
    ENVOY_LOG(error, "ack2: {}", ack2);
    if (ack2 != "ACK2") {
      ENVOY_LOG(error, "Received non-ACK2 from downstream listener");
      read_callbacks_->connection().close(Network::ConnectionCloseType::Abort);
      return Network::FilterStatus::StopIteration; 
    }

    // Connect to RDMA downstream (remote listener)
    rdma_sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (rdma_sock_ < 0) {
      ENVOY_LOG(error, "Failed to create RDMA socket");
      read_callbacks_->connection().close(Network::ConnectionCloseType::Abort);
      return Network::FilterStatus::StopIteration; 
    }

    // Print downstream IP + port
    Network::Connection& connection = read_callbacks_->connection();
    const auto& stream_info = connection.streamInfo();
    Network::Address::InstanceConstSharedPtr remote_address = stream_info.downstreamAddressProvider().remoteAddress();
    std::string downstream_ip = remote_address->ip()->addressAsString();

    // Connect to downstream
    uint32_t downstream_port = static_cast<uint32_t>(downstream_port_);
    struct sockaddr_in downstream_address_;
    downstream_address_.sin_family = AF_INET;
    downstream_address_.sin_port = htons(downstream_port);
    ENVOY_LOG(info, "IP: {}, Port: {}", downstream_ip, downstream_port);

    if (inet_pton(AF_INET, downstream_ip.c_str(), &downstream_address_.sin_addr) <= 0) {
      ENVOY_LOG(error, "Invalid address/Address not supported");
      read_callbacks_->connection().close(Network::ConnectionCloseType::Abort);
      return Network::FilterStatus::StopIteration; 
    }

    int res = connect(rdma_sock_, reinterpret_cast<struct sockaddr*>(&downstream_address_), sizeof(downstream_address_));
    if (res < 0) {
      ENVOY_LOG(error, "RDMA Failed to connect to downstream");
      ENVOY_LOG(error, "res: {}, {}", res, errno);
      read_callbacks_->connection().close(Network::ConnectionCloseType::FlushWrite);
      return Network::FilterStatus::StopIteration;
    }
    ENVOY_LOG(debug, "CONNECTED TO DOWNSTREAM");

    // Launch reponses from upstream handler
    std::thread upstream_to_downstream_thread(&ReceiverFilter::upstream_to_downstream, this);
    upstream_to_downstream_thread.detach();
    
    // Launch requests from downstream handler
    std::thread downstream_to_upstream_thread(&ReceiverFilter::downstream_to_upstream, this);
    downstream_to_upstream_thread.detach();
  }
  flag_++;
  data.drain(data.length());
  return Network::FilterStatus::StopIteration;
} 

} // namespace Receiver
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
