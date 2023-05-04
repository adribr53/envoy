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
#include <thread>

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace Receiver {

// Event called when receiving new client connection
Network::FilterStatus ReceiverFilter::onNewConnection() {
    ENVOY_LOG(debug, "onNewConnection triggered");
    // setup_connection_thread_ = std::thread(&ReceiverFilter::setup_connection, this);
    setup_connection();
    return Network::FilterStatus::Continue;
}

// Event called when receiving data from the downstream proxy (through the listener)
Network::FilterStatus ReceiverFilter::onData(Buffer::Instance& data, bool end_stream) {
    ENVOY_CONN_LOG(trace, "receiver: got {} bytes", read_callbacks_->connection(), data.length());

    // Downstream proxy closed the connection
    if ((end_stream == true || data.length() == 0) && !connection_close_) {
        ENVOY_LOG(info, "Downstream proxy closed the connection");
        // Terminate threads and connections
        close_procedure();
        // Do not go further in the filter chain
        return Network::FilterStatus::StopIteration;
    }

    // Drain read buffer
    data.drain(data.length());

    // Do not go further in the filter chain
    return Network::FilterStatus::StopIteration;
} 

// Event called when receiving data from the server (through tcp_proxy)
Network::FilterStatus ReceiverFilter::onWrite(Buffer::Instance& data, bool end_stream) {
    ENVOY_CONN_LOG(trace, "receiver: got {} bytes", write_callbacks_->connection(), data.length());
    ENVOY_LOG(debug, "written data: {}, end_stream: {}", data.toString(), end_stream);

    // Server closed the connection
    if (end_stream == true && !connection_close_) {
        ENVOY_LOG(info, "Server closed the connection");

        // Terminate threads and close filter connections
        close_procedure();

        // Drain write buffer
        data.drain(data.length());

        // End message to signal downstream proxy to close connection
        Buffer::OwnedImpl buff_end("end"); // End message to signal downstream proxy to close connection
        data.add(buff_end);

        // End message propagated to the downstream proxy through the listener
        return Network::FilterStatus::Continue;
    }
    
    // Push received data to circular buffer
    bool pushed = upstream_to_downstream_buffer_->push(data.toString());
    if (!pushed) {
        ENVOY_LOG(error, "upstream_to_downstream_buffer_ is currently full");
        if (!connection_close_) {
            ENVOY_LOG(info, "Closed due to full upstream_to_downstream_buffer_");
            close_procedure();
        }
    }

    // Drain write buffer
    data.drain(data.length());

    // Do not go further in the filter chain
    return Network::FilterStatus::StopIteration;
} 

} // namespace Receiver
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
