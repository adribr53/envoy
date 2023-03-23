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

Network::FilterStatus ReceiverFilter::onNewConnection() {
    ENVOY_LOG(debug, "onNewConnection triggered");
    return Network::FilterStatus::Continue;
}

Network::FilterStatus ReceiverFilter::onData(Buffer::Instance& data, bool end_stream) {
    ENVOY_CONN_LOG(trace, "receiver: got {} bytes", read_callbacks_->connection(), data.length());

    // Downstream listener closed the connection
    if (end_stream == true && !connection_close_) {
        ENVOY_LOG(info, "Downstream listener closed the connection");

        // Flag set to false to stop active threads
        active_rdma_polling_ = false;
        active_rdma_sender_ = false;
        active_upstream_sender_ = false;

        // Wait for all threads to finish
        if (rdma_polling_thread_ != nullptr) {
            rdma_polling_thread_.get()->join();
            rdma_polling_thread_ = nullptr;
        }
        if (rdma_sender_thread_ != nullptr) {
            rdma_sender_thread_.get()->join();
            rdma_sender_thread_ = nullptr;
        }
        if (upstream_sender_thread_ != nullptr) {
            upstream_sender_thread_.get()->join();
            upstream_sender_thread_ = nullptr;
        }

        ENVOY_LOG(info, "All threads terminated");
        connection_close_ = true;

        // Forward end_stream to further filter
        return Network::FilterStatus::Continue;
    }

    else if (connection_init_) {
        ENVOY_LOG(info, "Connection init");
        ENVOY_LOG(debug, "read data: {}, end_stream: {}", data.toString(), end_stream);
        std::string port = data.toString();
        ENVOY_LOG(debug, "port: {}", port);

        // Connect to RDMA downstream using received port
        struct sockaddr_in downstream_address_;
        uint32_t downstream_port = std::stoul(port);
        std::string downstream_ip = read_callbacks_->connection().streamInfo().downstreamAddressProvider().remoteAddress()->ip()->addressAsString();
        downstream_address_.sin_family = AF_INET;
        downstream_address_.sin_port = htons(downstream_port);
        ENVOY_LOG(debug, "DOWNSTREAM IP: {}, Port: {}", downstream_ip, downstream_port);
        inet_pton(AF_INET, downstream_ip.c_str(), &downstream_address_.sin_addr);

        // Try to connect to RDMA downstream
        int count = 0;
        sock_rdma_ = socket(AF_INET, SOCK_STREAM, 0);
        while ((connect(sock_rdma_, reinterpret_cast<struct sockaddr*>(&downstream_address_), sizeof(downstream_address_ ))) != 0) {
            if (count >= 10) {
                ENVOY_LOG(error, "RDMA Failed to connect to downstream");
                return Network::FilterStatus::StopIteration;
            }
            ENVOY_LOG(info, "RETRY CONNECTING TO RDMA DOWNSTREAM...");
            sleep(1);
            count++;
        }
        ENVOY_LOG(info, "CONNECTED TO RDMA DOWNSTREAM");

        // Launch RDMA polling thread
        rdma_polling_thread_ = thread_factory_.createThread([this]() {this->rdma_polling();}, absl::nullopt);

        // Launch RDMA sender thread
        rdma_sender_thread_ = thread_factory_.createThread([this]() {this->rdma_sender();}, absl::nullopt);

        // Launch upstream sender thread
        upstream_sender_thread_ = thread_factory_.createThread([this]() {this->upstream_sender();}, absl::nullopt);
        
        // downstream_to_upstream_buffer_.put(dataStr);
        connection_init_ = false;
    }

    else {
        ENVOY_LOG(debug, "Regular data");
        ENVOY_LOG(debug, "read data: {}, end_stream: {}", data.toString(), end_stream);
        bool pushed = false;
        while (!pushed) {
            pushed = downstream_to_upstream_buffer_.push(data.toString());
        }
    }

    data.drain(data.length());
    return Network::FilterStatus::StopIteration;
} 

Network::FilterStatus ReceiverFilter::onWrite(Buffer::Instance& data, bool end_stream) {
    ENVOY_CONN_LOG(trace, "receiver: got {} bytes", write_callbacks_->connection(), data.length());
    ENVOY_LOG(debug, "written data: {}, end_stream: {}", data.toString(), end_stream);

    // Server closed the connection
    if (end_stream == true && !connection_close_) {
        ENVOY_LOG(info, "Server closed the connection");

        // if (data.length() != 0) {
        //     // Push last message followed by a end message
        //     bool pushed1 = false;
        //     while (!pushed1) {
        //         pushed1 = upstream_to_downstream_buffer_.push(data.toString());
        //     }
        // }
        // std::string end = "end";
        // bool pushed2 = false;
        // while (!pushed2) {
        //     pushed2 = upstream_to_downstream_buffer_.push(end);
        // }

        // Flag set to false to stop active threads
        active_rdma_polling_ = false;
        active_rdma_sender_ = false;
        active_upstream_sender_ = false;

        // Wait for all threads to finish
        if (rdma_polling_thread_ != nullptr) {
            rdma_polling_thread_.get()->join();
            rdma_polling_thread_ = nullptr;
        }
        if (rdma_sender_thread_ != nullptr) {
            rdma_sender_thread_.get()->join();
            rdma_sender_thread_ = nullptr;
        }
        if (upstream_sender_thread_ != nullptr) {
            upstream_sender_thread_.get()->join();
            upstream_sender_thread_ = nullptr;
        }

        ENVOY_LOG(info, "All threads terminated");
        connection_close_ = true;

        data.drain(data.length());
        Buffer::OwnedImpl buff_end("end"); // End message to signal downstream listener to close connection
        data.add(buff_end);
        return Network::FilterStatus::Continue;
    }
    
    else {
        // Push received data from server to circular buffer
        bool pushed = false;
        while (!pushed) {
            pushed = upstream_to_downstream_buffer_.push(data.toString());
        }
    }

    data.drain(data.length());
    return Network::FilterStatus::StopIteration;
} 

} // namespace Receiver
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
