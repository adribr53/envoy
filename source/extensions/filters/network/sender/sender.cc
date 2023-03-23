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

Network::FilterStatus SenderFilter::onNewConnection() {
    ENVOY_LOG(debug, "onNewConnection triggered");
    return Network::FilterStatus::Continue;
}

Network::FilterStatus SenderFilter::onData(Buffer::Instance& data, bool end_stream) {
    ENVOY_CONN_LOG(trace, "sender: got {} bytes", read_callbacks_->connection(), data.length());
    
    // Client closed the connection
    if (end_stream == true && !connection_close_) {
        ENVOY_LOG(info, "Client closed the connection");
        // Flag set to false to stop active threads
        active_rdma_polling_ = false;
        active_rdma_sender_ = false;
        active_downstream_sender_ = false;

        // Wait for all threads to finish
        if (rdma_polling_thread_ != nullptr) {
            rdma_polling_thread_.get()->join();
            rdma_polling_thread_ = nullptr;
        }
        if (rdma_sender_thread_ != nullptr) {
            rdma_sender_thread_.get()->join();
            rdma_sender_thread_ = nullptr;
        }
        if (downstream_sender_thread_!= nullptr) {
            downstream_sender_thread_.get()->join();
            downstream_sender_thread_ = nullptr;
        }

        ENVOY_LOG(info, "All threads terminated");
        // Do not go further in the filter chain
        connection_close_ = true;
        return Network::FilterStatus::Continue;
    }

    // Connection initialization done when receiving the first message of the client
    else if (connection_init_) {
        ENVOY_LOG(info, "Connection init");
        ENVOY_LOG(debug, "read data: {}, end_stream: {}", data.toString(), end_stream);

        sock_rdma_ = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = 0;  // 0 means that the system will assign a free port number
        bind(sock_rdma_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        getsockname(sock_rdma_, reinterpret_cast<struct sockaddr*>(&addr), &addr_len); // query the socket to find out which port it was bound to

        // Send port number to upstream listener
        std::string port = std::to_string(ntohs(addr.sin_port));
        Buffer::OwnedImpl port_number(port);
        read_callbacks_->injectReadDataToFilterChain(port_number, end_stream);
        ENVOY_LOG(debug, "port sent: {}", port);

        // Launch RDMA polling thread
        rdma_polling_thread_ = thread_factory_.createThread([this]() {this->rdma_polling();}, absl::nullopt);

        connection_init_ = false;
    }

    // Receive regular data from client
    else {
        ENVOY_LOG(debug, "Regular data");
        ENVOY_LOG(debug, "read data: {}, end_stream: {}", data.toString(), end_stream);
    }
    // Push received data in ciruclar buffer
    bool pushed = false;
    while (!pushed) {
        pushed = downstream_to_upstream_buffer_.push(data.toString());
    }

    // Drain read buffer
    data.drain(data.length());
    // Do not go further in the filter chain
    return Network::FilterStatus::StopIteration;
} 

Network::FilterStatus SenderFilter::onWrite(Buffer::Instance& data, bool end_stream) {
    ENVOY_CONN_LOG(trace, "sender: got {} bytes", write_callbacks_->connection(), data.length());
    ENVOY_LOG(info, "written data: {}, end_stream: {}", data.toString(), end_stream);

    // Upstream listener closed the connection
    if (data.toString() == "end" && !connection_close_) {
        ENVOY_LOG(info, "Upstream listener closed the connection");

        // Flag set to false to stop active threads
        active_rdma_polling_ = false;
        active_rdma_sender_ = false;
        active_downstream_sender_ = false;

        // Wait for all threads to finish
        if (rdma_polling_thread_ != nullptr) {
            rdma_polling_thread_.get()->join();
            rdma_polling_thread_ = nullptr;
        }
        if (rdma_sender_thread_ != nullptr) {
            rdma_sender_thread_.get()->join();
            rdma_sender_thread_ = nullptr;
        }
        if (downstream_sender_thread_!= nullptr) {
            downstream_sender_thread_.get()->join();
            downstream_sender_thread_ = nullptr;
        }

        ENVOY_LOG(info, "All threads terminated");
        connection_close_= true;
        write_callbacks_->connection().close(Network::ConnectionCloseType::FlushWrite);
    }

    data.drain(data.length());
    return Network::FilterStatus::StopIteration;
} 

} // namespace Sender
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
