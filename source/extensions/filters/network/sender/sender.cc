#include "source/extensions/filters/network/sender/sender.h"

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
namespace Sender {

// Event called when receiving new client connection
Network::FilterStatus SenderFilter::onNewConnection() {
    ENVOY_LOG(debug, "onNewConnection triggered");
    return Network::FilterStatus::Continue;
}

// Event called when receiving data from the client (through the listener)
Network::FilterStatus SenderFilter::onData(Buffer::Instance& data, bool end_stream) {
    ENVOY_CONN_LOG(trace, "sender: got {} bytes", read_callbacks_->connection(), data.length());
    
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
        ENVOY_LOG(debug, "read data: {}, end_stream: {}", data.toString(), end_stream);

        // Socket RDMA initialization
        sock_rdma_ = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = 0;  // 0 means that the system will assign a free port number
        bind(sock_rdma_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        getsockname(sock_rdma_, reinterpret_cast<struct sockaddr*>(&addr), &addr_len); // Query the socket to find out which port it was bound to

        // Send port number of RDMA socket to upstream proxy
        std::string port = std::to_string(ntohs(addr.sin_port));
        Buffer::OwnedImpl port_number(port);
        read_callbacks_->injectReadDataToFilterChain(port_number, end_stream);
        ENVOY_LOG(debug, "RDMA port: {}", port);

        // Launch RDMA polling thread
        rdma_polling_thread_ = thread_factory_.createThread([this]() {this->rdma_polling();}, absl::nullopt);

        // Connection init is now done
        connection_init_ = false;
    }

    // Push received data in circular buffer
    // Maybe we should not push the string representation of the buffer (toString()) but directly put the Buffer::InstancePtr for better performance
    push(downstream_to_upstream_buffer_, data.toString());

    // Drain read buffer
    data.drain(data.length());
    
    // Do not go further in the filter chain
    return Network::FilterStatus::StopIteration;
} 

// Event called when receiving data from the upstream proxy (through tcp_proxy)
Network::FilterStatus SenderFilter::onWrite(Buffer::Instance& data, bool end_stream) {
    ENVOY_CONN_LOG(trace, "sender: got {} bytes", write_callbacks_->connection(), data.length());
    ENVOY_LOG(info, "written data: {}, end_stream: {}", data.toString(), end_stream);

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

} // namespace Sender
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
