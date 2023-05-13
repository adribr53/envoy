#include "source/extensions/filters/network/receiver_rdma_write_nomultip_read/receiver_rdma_write_nomultip_read.h"

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
namespace ReceiverRDMAWriteNomultipRead {

// Event called when receiving new client connection
Network::FilterStatus ReceiverRDMAWriteNomultipReadFilter::onNewConnection() {
    ENVOY_LOG(debug, "onNewConnection triggered");

    // Init RDMA connection
    // setup_rdma();

    return Network::FilterStatus::Continue;
}

// Event called when receiving data from the downstream proxy (through the listener)
Network::FilterStatus ReceiverRDMAWriteNomultipReadFilter::onData(Buffer::Instance& data, bool end_stream) {
    ENVOY_CONN_LOG(trace, "receiver_rdma_write_multip_write: got {} bytes", read_callbacks_->connection(), data.length());

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
Network::FilterStatus ReceiverRDMAWriteNomultipReadFilter::onWrite(Buffer::Instance& data, bool end_stream) {
    ENVOY_CONN_LOG(trace, "receiver_rdma_write_multip_write: got {} bytes", write_callbacks_->connection(), data.length());
    ENVOY_LOG(debug, "written data: {}, end_stream: {}", data.toString(), end_stream);
    static int firstCo_ = 1;

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

    if (first_write_) {        
        ENVOY_LOG(debug, "Connection init");
        static infinity::requests::RequestToken myrequestTokenWrite(contextToWrite_);
        requestTokenWrite_ = &myrequestTokenWrite;
        // Connection init is now done
        connection_init_ = false;
        if (firstCo_) {
            ENVOY_LOG(debug, "FIRST CO");
            firstCo_ = 0;
        } else {
            ENVOY_LOG(debug, "CREATE NEW REQ TOKEN");
            myrequestTokenWrite.~RequestToken();  // Call destructor manually
            new (&myrequestTokenWrite) infinity::requests::RequestToken(contextToWrite_);  // Placement new, creates new object at the same location
            requestTokenWrite_ = &myrequestTokenWrite;
        }
        first_write_ = 0;
    }
    // Push received data in circular buffer in string chunks of size payloadBound_  
    // to test - make the write here
    while (data.length() > 0) {
        uint32_t bytes_to_process = (uint32_t) std::min(data.length(), (const uint64_t) payloadBound_);
        Buffer::OwnedImpl chunk_data;
        chunk_data.move(data, bytes_to_process);

        /*bool pushed = upstream_to_downstream_buffer_->push(chunk_data.toString());
        if (!pushed) {
            ENVOY_LOG(error, "upstream_to_downstream_buffer_ is currently full");
            if (!connection_close_) {
                ENVOY_LOG(info, "Closed due to full upstream_to_downstream_buffer_");
                close_procedure();
            }
        }*/
        char *curSegment = get_ith(remoteHead_, offset_);
        while (!can_write(offset_, hostLimit_)) {               
            if (!active_rdma_sender_) {
                ENVOY_LOG(info, "onData was left\n");
                // Do not go further in the filter chain
                return Network::FilterStatus::StopIteration;
            }
            ENVOY_LOG(debug, "there is still more to send\n");
        }      
        ENVOY_LOG(debug, "T4");  
        memcpy(curSegment+payloadBound_-bytes_to_process, chunk_data.toString().c_str(), bytes_to_process);
        set_toCheck(curSegment, '1');		
        set_length(curSegment, bytes_to_process);
        uint32_t writeOffset = 2*sizeof(uint32_t) + (segmentSize_ * offset_) + (payloadBound_-bytes_to_process);		
        uint32_t writeLength = sizeof(uint32_t) + sizeof(char)+bytes_to_process;		
        if (!unsignaled_) {
            qpToWrite_->write(remoteMemory_, writeOffset, remoteMemoryToken_, writeOffset, writeLength, infinity::queues::OperationFlags(), requestTokenWrite_);		
            requestTokenWrite_->waitUntilCompleted();
        } else {
            qpToWrite_->write(remoteMemory_, writeOffset, remoteMemoryToken_, writeOffset, writeLength, infinity::queues::OperationFlags(), NULL);
        }
        offset_ = (offset_ + 1) % circleSize_;		
        unsignaled_ = (unsignaled_ + 1) % 128;
    }

    // Drain write buffer
    data.drain(data.length());

    // Do not go further in the filter chain
    return Network::FilterStatus::StopIteration;
} 

} // namespace ReceiverRDMAWriteNomultipRead
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
