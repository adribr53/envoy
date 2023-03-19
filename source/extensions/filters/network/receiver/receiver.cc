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
        if (!connection_init_) {
            ENVOY_LOG(debug, "Connection init");
            ENVOY_LOG(debug, "read data: {}, end_stream: {}", data.toString(), end_stream);
            std::string input = data.toString();
            std::string portStr, dataStr;
            splitString(input, portStr, dataStr);
            ENVOY_LOG(debug, "portStr: {}, dataStr: {}", portStr, dataStr);

            // Connect to RDMA downstream using received port
            struct sockaddr_in downstream_address_;
            uint32_t downstream_port = std::stoul(portStr);
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
                ENVOY_LOG(error, "RETRY CONNECTING TO RDMA DOWNSTREAM...");
                sleep(1);
                count++;
            }
            ENVOY_LOG(debug, "CONNECTED TO RDMA DOWNSTREAM");

            // Launch RDMA polling thread
            std::thread rdma_polling(&ReceiverFilter::rdma_polling, this);
            rdma_polling.detach();

            // Launch RDMA sender thread
            std::thread rdma_sender(&ReceiverFilter::rdma_sender, this);
            rdma_sender.detach();

            // Launch upstream sender thread
            std::thread upstream_sender(&ReceiverFilter::upstream_sender, this);
            upstream_sender.detach();
            
            downstream_to_upstream_buffer_.put(dataStr);
            connection_init_ = true;
        }
        else {
            ENVOY_LOG(debug, "Regular data");
            ENVOY_LOG(debug, "read data: {}, end_stream: {}", data.toString(), end_stream);
            downstream_to_upstream_buffer_.put(data.toString());
        }
        data.drain(data.length());
        return Network::FilterStatus::StopIteration;
    } 

    Network::FilterStatus ReceiverFilter::onWrite(Buffer::Instance& data, bool end_stream) {
        ENVOY_CONN_LOG(trace, "receiver: got {} bytes", write_callbacks_->connection(), data.length());
        ENVOY_LOG(debug, "written data: {}, end_stream: {}", data.toString(), end_stream);
        upstream_to_downstream_buffer_.put(data.toString());
        //write_callbacks_->injectWriteDataToFilterChain(data, end_stream);
        data.drain(data.length());
        return Network::FilterStatus::StopIteration;
    } 

} // namespace Receiver
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
