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
    // test_rdma_thread_ = std::thread(&SenderRDMAFilter::test_rdma, this);
    // ENVOY_LOG(debug, "After launching thread");
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
        test_rdma_thread_ = std::thread(&SenderRDMAFilter::test_rdma, this);
    //     ENVOY_LOG(debug, "read data: {}, end_stream: {}", data.toString(), end_stream);

    //     // Socket RDMA initialization
    //     sock_rdma_ = socket(AF_INET, SOCK_STREAM, 0);
    //     if (sock_rdma_ < 0) {
    //         ENVOY_LOG(error, "creating sock_rdma error");
    //         if (!connection_close_) {
    //             ENVOY_LOG(info, "Closed due to creating sock_rdma error");
    //             close_procedure();
    //         }
    //         return Network::FilterStatus::StopIteration;
    //     }

    //     struct sockaddr_in addr;
    //     socklen_t addr_len = sizeof(addr);
    //     addr.sin_family = AF_INET;
    //     addr.sin_addr.s_addr = htonl(INADDR_ANY);
    //     addr.sin_port = 0;  // 0 means that the system will assign a free port number
    //     if (bind(sock_rdma_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    //         ENVOY_LOG(error, "bind error");
    //         if (!connection_close_) {
    //             ENVOY_LOG(info, "Closed due to bind error");
    //             close_procedure();
    //         }
    //         return Network::FilterStatus::StopIteration;
    //     }
    //     if (getsockname(sock_rdma_, reinterpret_cast<struct sockaddr*>(&addr), &addr_len) < 0) { // Query the socket to find out which port it was bound to
    //         ENVOY_LOG(error, "getsockname error");
    //         if (!connection_close_) {
    //             ENVOY_LOG(info, "Closed due to getsockname error");
    //             close_procedure();
    //         }
    //         return Network::FilterStatus::StopIteration;
    //     }
    //     // Send port number of RDMA socket to upstream proxy
    //     std::string port = std::to_string(ntohs(addr.sin_port));
    //     Buffer::OwnedImpl port_number(port);
    //     read_callbacks_->injectReadDataToFilterChain(port_number, end_stream);
    //     ENVOY_LOG(info, "RDMA port: {}", port);

    //     // Launch RDMA polling thread
    //     rdma_polling_thread_ = std::thread(&SenderRDMAFilter::rdma_polling, this);

        // Connection init is now done
        connection_init_ = false;
    }

    // Push received data in circular buffer
    // Maybe we should not push the string representation of the buffer (toString()) but directly put the Buffer::InstancePtr for better performance    
    
    // TO TEST : replace by write
    bool pushed = downstream_to_upstream_buffer_->push(data.toString());
    if (!pushed) {
        ENVOY_LOG(error, "downstream_to_upstream_buffer_ is currently full");
        if (!connection_close_) {
            ENVOY_LOG(info, "Closed due to full downstream_to_upstream_buffer_");
            close_procedure();
        }
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
