#include "source/extensions/filters/network/receiver_rdma/receiver_rdma.h"

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
namespace ReceiverRDMA {

// Event called when receiving new client connection
Network::FilterStatus ReceiverRDMAFilter::onNewConnection() {
    ENVOY_LOG(debug, "onNewConnection triggered");

    // Init RDMA connection
    setup_rdma();

    return Network::FilterStatus::Continue;
}

// Event called when receiving data from the downstream proxy (through the listener)
Network::FilterStatus ReceiverRDMAFilter::onData(Buffer::Instance& data, bool end_stream) {
    ENVOY_CONN_LOG(trace, "receiver_rdma: got {} bytes", read_callbacks_->connection(), data.length());

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
Network::FilterStatus ReceiverRDMAFilter::onWrite(Buffer::Instance& data, bool end_stream) {
    ENVOY_CONN_LOG(trace, "receiver_rdma: got {} bytes", write_callbacks_->connection(), data.length());
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
    
    // Push received data in circular buffer in string chunks of size payloadBound_  
    // to test - make the write here
    while (data.length() > 0) {
        uint64_t bytes_to_process = std::min(data.length(), payloadBound_);
        Buffer::OwnedImpl chunk_data;
        chunk_data.move(data, bytes_to_process);

        bool pushed = upstream_to_downstream_buffer_->push(chunk_data.toString());
        if (!pushed) {
            ENVOY_LOG(error, "upstream_to_downstream_buffer_ is currently full");
            if (!connection_close_) {
                ENVOY_LOG(info, "Closed due to full upstream_to_downstream_buffer_");
                close_procedure();
            }
        }
    }

    // Drain write buffer
    data.drain(data.length());

    // Do not go further in the filter chain
    return Network::FilterStatus::StopIteration;
} 

} // namespace ReceiverRDMA
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
