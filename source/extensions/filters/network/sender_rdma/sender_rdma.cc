#include "source/extensions/filters/network/sender_rdma/sender_rdma.h"

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
namespace SenderRDMA {

// Event called when receiving new client connection
Network::FilterStatus SenderRDMAFilter::onNewConnection() {
    ENVOY_LOG(debug, "onNewConnection triggered");
    // TODO : launch setup_rdma() here if possible
    return Network::FilterStatus::Continue;
}

// Event called when receiving data from the client (through the listener)
Network::FilterStatus SenderRDMAFilter::onData(Buffer::Instance& data, bool end_stream) {
    ENVOY_CONN_LOG(trace, "sender_rdma: got {} bytes", read_callbacks_->connection(), data.length());
    
    // Client closed the connection
    if (end_stream == true && !connection_close_) {
        ENVOY_LOG(info, "Client closed the connection");

        // Terminate threads and close filter connections
        close_procedure();
        // Do not go further in the filter chain
        return Network::FilterStatus::StopIteration;
    }

    // Connection initialization done when receiving the first message from the client
    else if (connection_init_) {
        ENVOY_LOG(info, "Connection init");
        setup_rdma();

        // Connection init is now done
        connection_init_ = false;
    }

    // Push received data in circular buffer
    // Maybe we should not push the string representation of the buffer (toString()) but directly put the Buffer::InstancePtr for better performance    
    
    // TO TEST : replace by write

    ENVOY_LOG(debug, "data length: {}", data.length());
    const uint64_t chunk_size = 1500; // chunk size in bytes
    uint64_t bytes_processed = 0;
    while (data.length() > 0) {
        // Get the next chunk of data
        uint64_t bytes_to_process = std::min(data.length(), chunk_size);
        Buffer::OwnedImpl chunk_data;
        // data.move(chunk_data, bytes_to_process);
        chunk_data.move(data, bytes_to_process);
        ENVOY_LOG(debug, "data length: {}", data.length());

        bool pushed = downstream_to_upstream_buffer_->push(chunk_data.toString());
        if (!pushed) {
            ENVOY_LOG(error, "downstream_to_upstream_buffer_ is currently full");
            if (!connection_close_) {
                ENVOY_LOG(info, "Closed due to full downstream_to_upstream_buffer_");
                close_procedure();
            }
        }

        bytes_processed += bytes_to_process;
        ENVOY_LOG(debug, "bytes_processed: {}", bytes_processed);
    }

    // Drain read buffer
    data.drain(data.length());
    
    // Do not go further in the filter chain
    return Network::FilterStatus::StopIteration;
} 

// Event called when receiving data from the upstream proxy (through tcp_proxy)
Network::FilterStatus SenderRDMAFilter::onWrite(Buffer::Instance& data, bool end_stream) {
    ENVOY_CONN_LOG(trace, "sender_rdma: got {} bytes", write_callbacks_->connection(), data.length());
    ENVOY_LOG(debug, "written data: {}, end_stream: {}", data.toString(), end_stream);

    // Upstream proxy closed the connection
    if ((data.toString() == "end" || end_stream == true) && !connection_close_) {
        ENVOY_LOG(info, "Upstream proxy closed the connection");
        // Terminate threads and filter connections
        close_procedure();
    }

    // Drain write buffer
    data.drain(data.length());
    // Do not go further in the filter chain
    return Network::FilterStatus::StopIteration;
} 

} // namespace SenderRDMA
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
