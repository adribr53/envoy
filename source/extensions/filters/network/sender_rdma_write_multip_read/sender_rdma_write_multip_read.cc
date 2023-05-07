#include "source/extensions/filters/network/sender_rdma_write_multip_read/sender_rdma_write_multip_read.h"

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
namespace SenderRDMAWriteMultipRead {

// Event called when receiving new client connection
Network::FilterStatus SenderRDMAWriteMultipReadFilter::onNewConnection() {
    ENVOY_LOG(debug, "onNewConnection triggered");
    return Network::FilterStatus::Continue;
}

// Event called when receiving data from the client (through the listener)
Network::FilterStatus SenderRDMAWriteMultipReadFilter::onData(Buffer::Instance& data, bool end_stream) {
    ENVOY_CONN_LOG(trace, "sender_rdma_write_multip_read: got {} bytes", read_callbacks_->connection(), data.length());
    
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
        ENVOY_LOG(debug, "Connection init");

        // Init RDMA connection
        setup_rdma();

        // Connection init is now done
        connection_init_ = false;
    }

    // Push received data in circular buffer in string chunks of size payloadBound_   
    // TO TEST : replace by write
    while (data.length() > 0) {
        uint64_t bytes_to_process = std::min(data.length(), (const uint64_t) payloadBound_);
        Buffer::OwnedImpl chunk_data;
        chunk_data.move(data, bytes_to_process);

        bool pushed = downstream_to_upstream_buffer_->push(chunk_data.toString());
        if (!pushed) {
            ENVOY_LOG(error, "downstream_to_upstream_buffer_ is currently full");
            if (!connection_close_) {
                ENVOY_LOG(info, "Closed due to full downstream_to_upstream_buffer_");
                close_procedure();
            }
        }
    }

    // Drain read buffer
    data.drain(data.length());
    
    // Do not go further in the filter chain
    return Network::FilterStatus::StopIteration;
} 

// Event called when receiving data from the upstream proxy (through tcp_proxy)
Network::FilterStatus SenderRDMAWriteMultipReadFilter::onWrite(Buffer::Instance& data, bool end_stream) {
    ENVOY_CONN_LOG(trace, "sender_rdma_write_multip_read: got {} bytes", write_callbacks_->connection(), data.length());
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

} // namespace RDMAWriteMultipRead
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
