#include "source/extensions/filters/network/sender_rdma_write_nomultip_read/sender_rdma_write_nomultip_read.h"

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
namespace SenderRDMAWriteNomultipRead {

// Event called when receiving new client connection
Network::FilterStatus SenderRDMAWriteNomultipReadFilter::onNewConnection() {
    ENVOY_LOG(debug, "onNewConnection triggered");
    return Network::FilterStatus::Continue;
}

// Event called when receiving data from the client (through the listener)
Network::FilterStatus SenderRDMAWriteNomultipReadFilter::onData(Buffer::Instance& data, bool end_stream) {
    ENVOY_CONN_LOG(trace, "sender_rdma_write_nomultip_read: got {} bytes", read_callbacks_->connection(), data.length());
    // Client closed the connection
    static int firstCo_ = 1;
    if (end_stream == true && !connection_close_) {
        ENVOY_LOG(info, "Client closed the connection");

        // Terminate threads and close filter connections
        close_procedure();
        // Do not go further in the filter chain
        return Network::FilterStatus::StopIteration;
    }
    ENVOY_LOG(debug, "T1");
    
    // Connection initialization done when receiving the first message from the client
    if (connection_init_) {        
        ENVOY_LOG(debug, "Connection init");

        // Init RDMA connection
        setup_rdma();
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
    }
     // TODO : place smwhere else

    // Push received data in circular buffer in string chunks of size payloadBound_   
    // TO TEST : replace by write
    ENVOY_LOG(debug, "T2");
    
    while (data.length() > 0) {
        ENVOY_LOG(debug, "T3");
        uint32_t bytes_to_process = (uint32_t) std::min(data.length(), (const uint64_t) payloadBound_);
        Buffer::OwnedImpl chunk_data;
        chunk_data.move(data, bytes_to_process);

        /*bool pushed = downstream_to_upstream_buffer_->push(chunk_data.toString());
        if (!pushed) {
            ENVOY_LOG(error, "downstream_to_upstream_buffer_ is currently full");
            if (!connection_close_) {
                ENVOY_LOG(info, "Closed due to full downstream_to_upstream_buffer_");
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

    // Drain read buffer
    data.drain(data.length());
    
    // Do not go further in the filter chain
    return Network::FilterStatus::StopIteration;
} 

// Event called when receiving data from the upstream proxy (through tcp_proxy)
Network::FilterStatus SenderRDMAWriteNomultipReadFilter::onWrite(Buffer::Instance& data, bool end_stream) {
    ENVOY_CONN_LOG(trace, "sender_rdma_write_nomultip_read: got {} bytes", write_callbacks_->connection(), data.length());
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