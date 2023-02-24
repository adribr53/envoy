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
  ENVOY_LOG(debug, "TEST LOG");
  //std::string str = "Hello world ";
  //absl::string_view sv = str;
  //data.prepend(sv);
  std::string output = data.toString();
  ENVOY_LOG(debug, output);
  //read_callbacks_->connection().write(data, end_stream);
  ENVOY_LOG(debug, end_stream);
  //ASSERT(0 == data.length());

  int message_length = data.length();
  int bytes_sent = send(sock_, data.toString().c_str(), message_length, 0);
  if (bytes_sent != message_length) {
      ENVOY_LOG(error, "Failed to send message to upstream server");
      data.drain(data.length());
      return Envoy::Network::FilterStatus::Continue;
  }

  data.drain(data.length());
  return Network::FilterStatus::StopIteration;
} 

} // namespace Echo
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
