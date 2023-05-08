#pragma once

#include "envoy/network/filter.h"
#include "envoy/buffer/buffer.h"
#include "envoy/network/connection.h"
#include "envoy/event/dispatcher.h"

#include "source/common/common/logger.h"
#include "source/common/buffer/buffer_impl.h"

#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/poll.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <string>
#include <chrono>

#include <infinity/core/Context.h>
#include <infinity/queues/QueuePairFactory.h>
#include <infinity/queues/QueuePair.h>
#include <infinity/memory/Buffer.h>
#include <infinity/memory/RegionToken.h>
#include <infinity/requests/RequestToken.h>

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace ReceiverRDMAWriteMultipWrite {

/**
 * Implementation of a custom RDMA receiver filter
 */
class ReceiverRDMAWriteMultipWriteFilter : 
                       public Network::ReadFilter,
                       public Network::WriteFilter,
                       public Network::ConnectionCallbacks,
                       public std::enable_shared_from_this<ReceiverRDMAWriteMultipWriteFilter>,
                       Logger::Loggable<Logger::Id::filter> {
public:
    // Network::ReadFilter
    Network::FilterStatus onData(Buffer::Instance& data, bool end_stream) override;
    Network::FilterStatus onNewConnection() override;
    void initializeReadFilterCallbacks(Network::ReadFilterCallbacks& callbacks) override {
        read_callbacks_ = &callbacks; // For receiving data on the filter from downstream
        dispatcher_ = &read_callbacks_->connection().dispatcher();
        read_callbacks_->connection().addConnectionCallbacks(*this);
    }

    // Network::WriteFilter
    Network::FilterStatus onWrite(Buffer::Instance& data, bool end_stream) override;
    void initializeWriteFilterCallbacks(Network::WriteFilterCallbacks& callbacks) override {
        write_callbacks_ = &callbacks; // For receiving data on the filter from upstream
        write_callbacks_->connection().addConnectionCallbacks(*this);
    }

    // Network::ConnectionCallbacks
    void onEvent(Network::ConnectionEvent event) override {
        if (event == Network::ConnectionEvent::Connected) {
            if (connection_init_ && write_callbacks_->connection().state() == Network::Connection::State::Open) {
                ENVOY_LOG(info, "write_callbacks_ OPEN");
                setup_rdma();
                connection_init_ = false;
            }
        }

        if (event == Network::ConnectionEvent::RemoteClose ||
            event == Network::ConnectionEvent::LocalClose) {
                if (read_callbacks_->connection().state() == Network::Connection::State::Closed || // Downstream connection closed
                    read_callbacks_->connection().state() == Network::Connection::State::Closing) {
                        ENVOY_LOG(info, "read_callbacks_ CLOSED");
                        if (!connection_close_) {
                            ENVOY_LOG(info, "Close in read_callbacks_ event");
                            close_procedure();
                        }
                }
                if (write_callbacks_->connection().state() == Network::Connection::State::Closed || // Upstream connection closed
                    write_callbacks_->connection().state() == Network::Connection::State::Closing) {
                    ENVOY_LOG(info, "write_callbacks_ CLOSED");
                    if (!connection_close_) {
                        ENVOY_LOG(info, "Close in write_callbacks_ event");
                        close_procedure();
                    }
                }
        }
    }

    // Called when the write buffer becomes too full (reaches a threshold)
    // We may want to pause upstream traffic in this case
    void onAboveWriteBufferHighWatermark() override {
        ENVOY_LOG(info, "onAboveWriteBufferHighWatermark triggered");
        write_callbacks_->connection().readDisable(true);
    }

    // Called when number of items in write buffer is below a threshold 
    // We may want to resume upstream traffic in this case
    void onBelowWriteBufferLowWatermark() override {
        ENVOY_LOG(info, "onBelowWriteBufferLowWatermark triggered");
        write_callbacks_->connection().readDisable(false);
    }
  
    // Destructor
    ~ReceiverRDMAWriteMultipWriteFilter() {
        ENVOY_LOG(info, "DESTRUCTOR");
        // Free resources
        delete qpFactoryToPoll_;
        delete contextToPoll_;
        delete qpToPoll_;
        delete qpFactoryToWrite_;
        delete contextToWrite_;
        delete qpToWrite_;
        delete hostMemory_;
        delete remoteMemory_;
    }

    // Constructor
    ReceiverRDMAWriteMultipWriteFilter(const uint32_t payloadBound, const uint32_t circleSize, const uint32_t timeToWrite, const uint32_t sharedBufferSize)
        : payloadBound_(payloadBound), circleSize_(circleSize), timeToWrite_(timeToWrite), sharedBufferSize_(sharedBufferSize)
    {
        ENVOY_LOG(info, "CONSTRUCTOR CALLED");
        ENVOY_LOG(info, "payloadBound_: {}", payloadBound_);
        ENVOY_LOG(info, "circleSize_: {}", circleSize_);
        ENVOY_LOG(info, "timeToWrite_: {}", timeToWrite_);
        ENVOY_LOG(info, "sharedBufferSize_: {}", sharedBufferSize_);

        downstream_to_upstream_buffer_ = std::make_shared<CircularBuffer<std::string>>(sharedBufferSize_);
        upstream_to_downstream_buffer_ = std::make_shared<CircularBuffer<std::string>>(sharedBufferSize_);
        segmentSize_ = sizeof(uint32_t) + sizeof(char) + payloadBound_;
        bufferSize_ = (circleSize_ * segmentSize_ ) + sizeof(uint8_t);
    }

    ///////////////////////////
    // bunch of utils for RDMA
    ///////////////////////////
    int can_write(uint8_t offset, uint8_t limit) {	
	    return offset != limit;
    }

    char get_toCheck(volatile char *cur) {
        cur = cur + payloadBound_ + sizeof(uint32_t);
        return *cur;
    }

    void set_toCheck(volatile char *cur, char v) {
        cur = cur + payloadBound_ + sizeof(uint32_t);
        *cur = v;
    }

    uint32_t get_length(volatile char *cur) {
        cur = cur + payloadBound_;
        return ntohl(*((uint32_t *)cur));
    }

    uint32_t get_length(char *cur) {	
        cur = cur + payloadBound_;
        return ntohl(*((uint32_t *)cur));
    }

    void set_length(volatile char *cur, uint32_t length) {
        uint32_t *ptr = (uint32_t *) (cur + payloadBound_);
        *ptr = htonl(length);
    }

    volatile char *get_payload(volatile char *cur) {
        uint32_t length = get_length(cur);
        cur = cur + payloadBound_ - length;
        return cur;
    }

    char *get_payload(char *cur) {
        uint32_t length = get_length(cur);
        cur = cur + payloadBound_ - length;
        return cur;
    }

    volatile char *get_ith(volatile char *head, uint32_t i) {
        volatile char *ith = head + i * segmentSize_;
        return ith;
    }

    char *get_ith(char *head, uint32_t i) {	
        char *ith = head+i*segmentSize_;
        return ith;
    }

    int time_to_write(uint8_t curLimit, uint8_t *remoteLimit_) {
        //printf("%u %u\n", curLimit, *remoteLimit_);
        if (curLimit == *remoteLimit_) return 0;
        if (curLimit < *remoteLimit_) {
                return *remoteLimit_-curLimit < circleSize_/4;}
        else {// remoteLimit_ 109, curLimit 173
                return curLimit-*remoteLimit_ > circleSize_/4;
        }
    }
    
    // This function is responsible for initializing the RDMA conneciton in both directions
    void setup_rdma() {
        ENVOY_LOG(debug, "launch setup_rdma");

        // Get source IP and source Port of downstream connection
        const auto& stream_info = read_callbacks_->connection().streamInfo();
        Network::Address::InstanceConstSharedPtr remote_address = stream_info.downstreamAddressProvider().remoteAddress();
        std::string source_ip = remote_address->ip()->addressAsString();
        uint32_t source_port = remote_address->ip()->port();
        ENVOY_LOG(debug, "Source IP: {}, Source Port {}", source_ip, source_port);
     
        uint32_t portNumber = source_port+1;
        
        // SETUP 1
		contextToPoll_ = new infinity::core::Context();
		qpFactoryToPoll_ = new infinity::queues::QueuePairFactory(contextToPoll_);
		hostMemory_ = new infinity::memory::Buffer(contextToPoll_, bufferSize_); // todo : one more case for reader head
		hostMemoryToken_ = hostMemory_->createRegionToken();
		volatile char *hostBuffer = (char *) hostMemory_->getData();
		hostLimit_ = (uint8_t *) hostBuffer;
		*hostLimit_ = circleSize_-1;
		hostHead_ = hostBuffer+sizeof(uint8_t); 		 		
		for (uint8_t i = 1; i <= circleSize_; i++) {
			volatile char *ith = get_ith(hostHead_, i);
			set_toCheck(ith, '0');
		}
		qpFactoryToPoll_->bindToPort(portNumber);
		qpToPoll_ = qpFactoryToPoll_->acceptIncomingConnection(hostMemoryToken_, sizeof(infinity::memory::RegionToken));
        ENVOY_LOG(debug, "ACCEPTED RDMA");
		
		// SETUP 2
		contextToWrite_ = new infinity::core::Context();
		qpFactoryToWrite_ = new infinity::queues::QueuePairFactory(contextToWrite_);
		qpToWrite_ = qpFactoryToWrite_->connectToRemoteHost(source_ip.c_str(), portNumber);
		remoteMemoryToken_ = (infinity::memory::RegionToken *) qpToWrite_->getUserData();
		remoteMemory_ = new infinity::memory::Buffer(contextToWrite_, bufferSize_);
		remoteBuffer_ = (char *) remoteMemory_->getData();
		remoteHead_ = remoteBuffer_ + sizeof(uint8_t);
		remoteLimit_ = (uint8_t *) remoteBuffer_;

        ENVOY_LOG(debug, "CONNECTED RDMA");

        rdma_polling_thread_ = std::thread(&ReceiverRDMAWriteMultipWriteFilter::rdma_polling, this);        
        rdma_sender_thread_ = std::thread(&ReceiverRDMAWriteMultipWriteFilter::rdma_sender, this);
        upstream_sender_thread_ = std::thread(&ReceiverRDMAWriteMultipWriteFilter::upstream_sender, this);
    }    

    // This function will run in a thread and be responsible for RDMA polling
    void rdma_polling() {
        ENVOY_LOG(info, "rdma_polling started");
        while (true) {
            uint8_t curOffset = 0;		
            uint8_t curLimit = circleSize_ - 1;
            *remoteLimit_ = curLimit;	

            clock_t lastTime = clock();
            infinity::requests::RequestToken requestTokenWriteControl(contextToWrite_);
            while (true) {
                volatile char *ith = get_ith(hostHead_, curOffset);

                if (3000000 < clock() - lastTime) {	
                    if (!active_rdma_polling_) {
                        // do a write here to allow finish on envoy ?
                        ENVOY_LOG(info, "rdma_polling stopped");
                        return;
                    } 						
                    // break;
                }
                
                if (get_toCheck(ith) == '1') {
                    //printf("data arrived %u %u\n", curLimit, *remoteLimit_);
                    set_toCheck(ith, '0');		
                    std::string message((char*) get_payload(ith), get_length(ith)); // Put the received data in a string                
                    // Push the data in the circular buffer
                    ENVOY_LOG(debug, "received message: {}", message);
                    bool pushed = downstream_to_upstream_buffer_->push(message);
                    if (!pushed) {
                        ENVOY_LOG(error, "upstream_to_downstream_buffer_ is currently full");
                        if (!connection_close_) {
                            ENVOY_LOG(info, "Closed due to full upstream_to_downstream_buffer_");
                            close_procedure();
                        }
                        break;
                    }
                    curOffset = (curOffset+1) % circleSize_;
                    curLimit = (curLimit+1) % circleSize_;
                    if (time_to_write(curLimit, remoteLimit_)) {
                        //printf("time to write %u %u\n", curLimit, *remoteLimit_);
                        //qpToWrite_->read(remoteMemory_, remoteMemoryToken_, sizeof(uint8_t), &requestTokenRead);
                        *remoteLimit_ = curLimit;
                        qpToWrite_->write(remoteMemory_, 0, remoteMemoryToken_, 0, sizeof(uint8_t), infinity::queues::OperationFlags(), &requestTokenWriteControl);		
                        requestTokenWriteControl.waitUntilCompleted();
                    }
                    lastTime = clock();
                }		
            }
        }
   
        ENVOY_LOG(debug, "rdma_polling stopped");
    }

    // This function will run in a thread and be responsible for sending to downstream through RDMA
    void rdma_sender() {
        ENVOY_LOG(info, "rdma_sender launched");
        char *curSegment;
    	uint8_t offset = 0;
	    infinity::requests::RequestToken requestTokenWrite(contextToWrite_);

        while (true) {
            if (!can_write(offset, *hostLimit_)) {
                if (!active_rdma_sender_ && downstream_to_upstream_buffer_->getSize() == 0) {
                    break;
                }
                else {
                    continue; // maybe need to force write on the receiver when co is done to allow for termination of this
                }
            }
            curSegment = get_ith(remoteHead_, offset);	

            std::string item;
            if (upstream_to_downstream_buffer_->pop(item)) { // to opti
                ENVOY_LOG(debug, "Got item: {}", item);

                // TO USE IF put directly in buffer
                // if (length!=payloadBound_) {
                //     // that, or checksum, of offload sending unused bytes to the nic
                //     int diff = payloadBound_-length;
                //     for (int i=length-1; i>=0; i--) {
                //         curSegment[i+diff] = curSegment[i];
                //     }
                // }
                ssize_t length = item.size();
                memcpy(curSegment+payloadBound_-item.size(), item.c_str(), length);
                set_toCheck(curSegment, '1');		
                set_length(curSegment, length);		
                uint32_t writeOffset = sizeof(uint8_t) + (segmentSize_ * offset) + (payloadBound_-length);		
                uint32_t writeLength = sizeof(uint32_t)+sizeof(char)+length;		
                if (!offset) {
                    qpToWrite_->write(remoteMemory_, writeOffset, remoteMemoryToken_, writeOffset, writeLength, infinity::queues::OperationFlags(), &requestTokenWrite);		
                    requestTokenWrite.waitUntilCompleted();
                } 
                else {
                    qpToWrite_->write(remoteMemory_, writeOffset, remoteMemoryToken_, writeOffset, writeLength, infinity::queues::OperationFlags(), NULL);
                }
                offset = (offset + 1) % circleSize_;
            } 	
            else { // No item was retrieved after timeout_value seconds
                if (!active_rdma_sender_) { // If timeout and flag false: stop thread
                    break;
                }
            }            		
        }
        ENVOY_LOG(info, "rdma_sender stopped");
    }

    // This function will run in a thread and be responsible for sending requests to the server through the dispatcher
    void upstream_sender() {
        ENVOY_LOG(info, "upstream_sender launched");
        while (true) {
            std::string item;
            if (downstream_to_upstream_buffer_->pop(item)) {
                ENVOY_LOG(debug, "Got item: {}", item);

                // Use dispatcher and locking to ensure that the right thread executes the task (sending requests to the server)
                // Asynchronous task
                // TO TEST : put this in rdma polling instead
                auto weak_self = weak_from_this();
                dispatcher_->post([weak_self, buffer = std::make_shared<Buffer::OwnedImpl>(item)]() -> void {
                    if (auto self = weak_self.lock()) {
                        self->read_callbacks_->injectReadDataToFilterChain(*buffer, false); // Inject data to tcp_proxy
                    }
                });
            }
            else { // No item was retrieved after timeout_value_ seconds
                if (!active_upstream_sender_) { // If timeout and flag false: stop thread
                    break;
                }
            }
        }
        ENVOY_LOG(info, "upstream_sender stopped");
    }

    // Handling termination of threads and close filter connections
    void close_procedure() {
        connection_close_ = true;

        // Flag set to false to stop active threads
        active_rdma_polling_ = false;
        active_rdma_sender_ = false;
        active_upstream_sender_ = false;

        // Wait for all threads to finish
        if (rdma_polling_thread_.joinable()) {
            rdma_polling_thread_.join();
        }
        if (rdma_sender_thread_.joinable()) {
            rdma_sender_thread_.join();
        }
        if (upstream_sender_thread_.joinable()) {
            upstream_sender_thread_.join();
        }
        ENVOY_LOG(info, "All threads terminated");

        ENVOY_LOG(info, "upstream_to_downstream_buffer_ size: {}", upstream_to_downstream_buffer_->getSize()); // Should always be 0
        ENVOY_LOG(info, "downstream_to_upstream_buffer_ size: {}", downstream_to_upstream_buffer_->getSize()); // Should always be 0

        // Close filter connections and flush pending write data
        if (read_callbacks_->connection().state() == Network::Connection::State::Open) {
            read_callbacks_->connection().close(Network::ConnectionCloseType::FlushWrite);
        }
        if (write_callbacks_->connection().state() == Network::Connection::State::Open) {
            write_callbacks_->connection().close(Network::ConnectionCloseType::FlushWrite);
        }
    }

    // Thread-safe Circular buffer
    // Operations: push() and pop() an item, getSize()
    #include <vector>
    #include <mutex>
    #include <condition_variable>
    #include <chrono>
    #include <atomic>

    template<typename T>
    class CircularBuffer {
    public:
        // Constructor
        CircularBuffer(size_t size) : buffer_(size), head_(0), tail_(0), size_(0) {}

        // Put an element in the buffer and return true if successful, false otherwise
        bool push(const T& item) {
            std::unique_lock<std::mutex> lock(mutex_);
            if (size_ == buffer_.size()) {
                return false; // buffer is full
            }
            buffer_[head_] = item;
            head_ = (head_ + 1) % buffer_.size();
            size_++;
            cv_.notify_one();
            return true;
        }

        // Get an element from the buffer and return true if successful, false if it did not pop any item after timeout_value_ seconds
        bool pop(T& item) {
            std::unique_lock<std::mutex> lock(mutex_);
            if (size_ == 0) {
                if (!cv_.wait_for(lock, std::chrono::seconds(timeout_value_), [this] { return size_ > 0; })) {
                    return false; // buffer is still empty after timeout_value_ seconds
                }
            }
            item = buffer_[tail_];
            tail_ = (tail_ + 1) % buffer_.size();
            size_--;
            return true;
        }

        // Get current size of the buffer
        int getSize() {
            std::lock_guard<std::mutex> lock(mutex_);
            return size_;
        }

    private:
        std::vector<T> buffer_;
        size_t head_;
        size_t tail_;
        size_t size_;
        std::mutex mutex_;
        std::condition_variable cv_;
    };

private:
    // Callbacks
    Network::ReadFilterCallbacks* read_callbacks_{}; // ReadFilter callback (handle data from downstream)
    Network::WriteFilterCallbacks* write_callbacks_{}; // WriterFilter callback (handle data from upstream)

    // Connection flags
    std::atomic<bool> connection_close_{false}; // Keep track of connection state
    std::atomic<bool> connection_init_{true};

    // Timeout
    const static uint32_t timeout_value_ = 1; // In seconds
      
    // RDMA stuff   
    const uint32_t payloadBound_;
    const uint32_t circleSize_;
    const uint32_t timeToWrite_;
    const uint32_t sharedBufferSize_;

    uint32_t segmentSize_;
    uint32_t bufferSize_;

    infinity::core::Context *contextToWrite_;
    infinity::queues::QueuePairFactory *qpFactoryToWrite_;
    infinity::queues::QueuePair *qpToWrite_;
    infinity::memory::RegionToken *remoteMemoryToken_;
    infinity::memory::Buffer *remoteMemory_;
    char *remoteBuffer_;
    char *remoteHead_;
    uint8_t *remoteLimit_;

    infinity::core::Context *contextToPoll_;
    infinity::queues::QueuePairFactory *qpFactoryToPoll_;
    infinity::queues::QueuePair *qpToPoll_;
    infinity::memory::Buffer *hostMemory_; // todo : one more case for reader head
    infinity::memory::RegionToken *hostMemoryToken_;
    volatile char *hostHead_; 
    volatile uint8_t* hostLimit_;

    // Buffers
    std::shared_ptr<CircularBuffer<std::string>> downstream_to_upstream_buffer_; // Buffer supplied by RDMA polling thread and consumed by the upstream sender thread
    std::shared_ptr<CircularBuffer<std::string>> upstream_to_downstream_buffer_; // Buffer supplied by onWrite() and consumed by RDMA sender thread

    // Dispatcher
    Envoy::Event::Dispatcher* dispatcher_{}; // Used to give the control back to the thread responsible for sending requests to the server (used in upstream_sender())

    // Threads
    std::thread rdma_polling_thread_;
    std::thread rdma_sender_thread_;
    std::thread upstream_sender_thread_;

    // Thread flags
    std::atomic<bool> active_rdma_polling_{true}; // If false, stop the thread
    std::atomic<bool> active_rdma_sender_{true}; // If false, stop the thread
    std::atomic<bool> active_upstream_sender_{true}; // If false, stop the thread

   };

} // namespace ReceiverRDMAWriteMultipWrite
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy