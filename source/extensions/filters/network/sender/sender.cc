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

        if (!connection_init_) {
            ENVOY_LOG(debug, "Connection init");
            ENVOY_LOG(debug, "read data: {}, end_stream: {}", data.toString(), end_stream);

            sock_rdma_ = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in addr;
            socklen_t addr_len = sizeof(addr);
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            addr.sin_port = 0;  // 0 means that the system will assign a free port number
            bind(sock_rdma_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
            // query the socket to find out which port it was bound to
            getsockname(sock_rdma_, reinterpret_cast<struct sockaddr*>(&addr), &addr_len);

            // Send port number to upstream listener
            std::string port = std::to_string(ntohs(addr.sin_port));
            Buffer::OwnedImpl port_number(port + "|");
            read_callbacks_->injectReadDataToFilterChain(port_number, end_stream);
            ENVOY_LOG(debug, "port: {}", port);

            // Launch RDMA polling thread
            std::thread rdma_polling(&SenderFilter::rdma_polling, this);
            rdma_polling.detach();

            connection_init_ = true;
        }
        else {
            ENVOY_LOG(debug, "Regular data");
            ENVOY_LOG(debug, "read data: {}, end_stream: {}", data.toString(), end_stream);
        }
        downstream_to_upstream_buffer_.put(data.toString());
        data.drain(data.length());
        return Network::FilterStatus::StopIteration;
    } 

    Network::FilterStatus SenderFilter::onWrite(Buffer::Instance& data, bool end_stream) {
        ENVOY_CONN_LOG(trace, "sender: got {} bytes", write_callbacks_->connection(), data.length());
        ENVOY_LOG(debug, "written data: {}, end_stream: {}", data.toString(), end_stream);
        //write_callbacks_->injectWriteDataToFilterChain(data, end_stream);
        data.drain(data.length());
        return Network::FilterStatus::StopIteration;
    } 

} // namespace Sender
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
