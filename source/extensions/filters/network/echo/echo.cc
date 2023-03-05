#include "source/extensions/filters/network/echo/echo.h"

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
namespace Echo {

Network::FilterStatus EchoFilter::onData(Buffer::Instance& data, bool end_stream) {
  ENVOY_CONN_LOG(trace, "echo: got {} bytes", read_callbacks_->connection(), data.length());
  
  // Print downstream IP + port
  Network::Connection& connection = read_callbacks_->connection();
  const auto& stream_info = connection.streamInfo();
  Network::Address::InstanceConstSharedPtr remote_address = stream_info.downstreamAddressProvider().remoteAddress();
  std::string source_ip = remote_address->ip()->addressAsString();
  uint32_t source_port = remote_address->ip()->port();
  ENVOY_LOG(debug, "Received {} bytes from {}:{}", data.length(), source_ip, source_port);

  std::string output = data.toString();
  ENVOY_LOG(debug, output);
  ENVOY_LOG(debug, end_stream);

  // Forward requests from downstream to upstream
  int message_length = data.length();
  int bytes_sent = send(sock_, data.toString().c_str(), message_length, 0);
  if (bytes_sent != message_length) {
      ENVOY_LOG(error, "Failed to send message to upstream server");
      data.drain(data.length());
      return Network::FilterStatus::Continue;
  }

  data.drain(data.length());
  return Network::FilterStatus::StopIteration;
} 

} // namespace Echo
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
