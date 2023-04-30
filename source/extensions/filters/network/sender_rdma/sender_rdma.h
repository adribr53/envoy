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
namespace SenderRDMA {

/**
 * Implementation of a custom TCP sender_rdma filter
 */
class SenderRDMAFilter : public Network::ReadFilter,
                     public Network::WriteFilter,
                     public Network::ConnectionCallbacks,
                     public std::enable_shared_from_this<SenderRDMAFilter>,
                     Logger::Loggable<Logger::Id::filter> {
public:
    // Network::ReadFilter
    Network::FilterStatus onData(Buffer::Instance& data, bool end_stream) override;
    Network::FilterStatus onNewConnection() override;
    void initializeReadFilterCallbacks(Network::ReadFilterCallbacks& callbacks) override {
        read_callbacks_ = &callbacks; // For receiving data on the filter from downstream
        read_callbacks_->connection().addConnectionCallbacks(*this);
    }

    // Network::WriteFilter
    Network::FilterStatus onWrite(Buffer::Instance& data, bool end_stream) override;
    void initializeWriteFilterCallbacks(Network::WriteFilterCallbacks& callbacks) override {
        write_callbacks_ = &callbacks; // For receiving data on the filter from upstream
        dispatcher_ = &write_callbacks_->connection().dispatcher();
        write_callbacks_->connection().addConnectionCallbacks(*this);
    }

    // Network::ConnectionCallbacks
    void onEvent(Network::ConnectionEvent event) override {
        if (event == Network::ConnectionEvent::RemoteClose ||
            event == Network::ConnectionEvent::LocalClose) {
                if (read_callbacks_->connection().state() == Network::Connection::State::Closed ||
                    read_callbacks_->connection().state() == Network::Connection::State::Closing) {
                    ENVOY_LOG(info, "read_callbacks_ CLOSED");
                    if (!connection_close_) {
                        ENVOY_LOG(info, "Close in read_callbacks_ event");
                        // Terminate threads and close filter connections
                        close_procedure();
                    }
                }
                if (write_callbacks_->connection().state() == Network::Connection::State::Closed ||
                        write_callbacks_->connection().state() == Network::Connection::State::Closing) {
                    ENVOY_LOG(info, "write_callbacks_ CLOSED");
                    if (!connection_close_) {
                        ENVOY_LOG(info, "Close in write_callbacks_ event");
                        // Terminate threads and close filter connections
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
    ~SenderRDMAFilter() {
        ENVOY_LOG(info, "DESTRUCTOR");
        close(sock_rdma_);
        close(sock_distant_rdma_);
    }

    // Constructor
    SenderRDMAFilter() {
        ENVOY_LOG(info, "CONSTRUCTOR CALLED");
        // test_rdma_thread_ = std::thread(&SenderRDMAFilter::test_rdma, this);
    }

    void set_length(volatile char *cur, uint32_t length) {
        uint32_t *ptr = (uint32_t *) (cur + sizeof(uint32_t) + sizeof(char));
        *ptr = htonl(length);
    }

    volatile char *get_ith(volatile char *head, uint32_t i) {
        volatile char *ith = head+i*segmentSize;
        return ith;
    }

    uint32_t get_length(volatile char *cur) {
        cur = cur + sizeof(uint32_t) + sizeof(char);
        return ntohl(*((uint32_t *)cur));
    }

    char get_toCheck(volatile char *cur) {
        cur = cur + sizeof(uint32_t);
        return *cur;
    }

    char *get_ith(char *head, uint32_t i) {	
        char *ith = head+i*segmentSize;
        return ith;
    }

    void set_toCheck(volatile char *cur, char v) {
        cur = cur + sizeof(uint32_t);
        *cur = v;
    }

    void test_rdma() {
        ENVOY_LOG(debug, "launch test_rdma");
        // Get destination_ip
        // Get source_port of write_callbacks
        Network::Connection& connection = write_callbacks_->connection();
        uint32_t source_port = write_callbacks_->connection().streamInfo().upstreamInfo().get()->upstreamLocalAddress()->ip()->port();
        std::string destination_ip = write_callbacks_->connection().streamInfo().upstreamInfo().get()->upstreamRemoteAddress()->ip()->addressAsString();
        ENVOY_LOG(debug, "Destination IP: {}, Source Port {}", destination_ip, source_port);

        // Create RDMA client socket and connects to (destination_ip, source_port+1)
        context = new infinity::core::Context();
        qpFactory = new infinity::queues::QueuePairFactory(context);        
        server_ip = destination_ip.c_str();
        port_number = source_port+1;
        circle_size = 100;
        segmentSize = 2*sizeof(uint32_t)+sizeof(char)+payloadBound;
        bufferSize = (circle_size * segmentSize )+sizeof(uint32_t);

        ENVOY_LOG(debug, "IP: {}", server_ip);
        ENVOY_LOG(debug, "PORT: {}", port_number);
        qpToWrite = qpFactory->connectToRemoteHost(server_ip, port_number);
        ENVOY_LOG(debug, "AFTER CONNECT");
        remoteMemoryToken = (infinity::memory::RegionToken *) qpToWrite->getUserData();
        ENVOY_LOG(debug, "AFTER GET USER DATA");
        ////printf("Creating buffers\n");
        remoteMemory = new infinity::memory::Buffer(context, bufferSize);
        remoteBuffer = (char *) remoteMemory->getData();
        ENVOY_LOG(debug, "AFTER REMOTE MEMORY GET DATA");
        remoteHead = remoteBuffer + sizeof(uint32_t);
        remotePosition = (uint32_t *) remoteBuffer;
        margin = (circle_size/2)-1;
        offset = 0;
        ENVOY_LOG(debug, "CONNECTION RDMA ESTABLISHED");
        ENVOY_LOG(debug, "CONNECTION RDMA HELLO");

        rdma_sender_thread_ = std::thread(&SenderRDMAFilter::rdma_sender, this);

        // let's create the other chanel
        hostMemory = new infinity::memory::Buffer(context, bufferSize); // todo : one more case for reader head
		hostMemoryToken = hostMemory->createRegionToken();
		hostBuffer = (char *) hostMemory->getData();
		hostOffset = (uint32_t *) hostBuffer;
		*hostOffset = 0;
		hostHead = hostBuffer+sizeof(uint32_t); // first bytes to store cur position
		//memset(head, '0', CIRCLE_SIZE * payloadBound * sizeof(char));
		//printf("Setting up connection I think (blocking)\n"); 		
		for (uint32_t i = 1; i<=circle_size; i++) {
 			//printf("cur : ");
			volatile char *ith = get_ith(hostHead, i);
			set_toCheck(ith, '0');
			//printf("%d ", i);
		}
        qpFactory->bindToPort(port_number);
		qpToPoll = qpFactory->acceptIncomingConnection(hostMemoryToken, sizeof(infinity::memory::RegionToken)); // todo : retrieve 4-tuple
        ENVOY_LOG(debug, "CONNECTION RDMA 2 ESTABLISHED");

        rdma_polling_thread_ = std::thread(&SenderRDMAFilter::rdma_polling, this);
        downstream_sender_thread_ = std::thread(&SenderRDMAFilter::downstream_sender, this);
    }

    void rdma_polling() {
        ENVOY_LOG(debug, "rdma_polling launched");
        uint32_t actualOffset = 0;		
	    uint32_t writtenOffset = 0;
	    int cnt = 0;
        
	    auto cur = std::chrono::high_resolution_clock::now();
	    while (1) {	
            volatile char *ith = get_ith(hostHead, actualOffset);
            auto ms = std::chrono::duration_cast<std::chrono::seconds>(cur.time_since_epoch()).count();
            if (ms>10) {
                break;
            }
            if (get_toCheck(ith)=='1') {                
                set_toCheck(ith, '0');
                *hostOffset = (*hostOffset+1) % circle_size;
                actualOffset = (actualOffset+1) % circle_size;
                if (actualOffset-writtenOffset>circle_size/2 || writtenOffset-actualOffset>circle_size/2) {
                    writtenOffset = actualOffset;
                    // write in the other's memory 
                } 			   

                std::string message((char*) get_payload(ith), get_length(ith)); // Put the received data in a string
                ENVOY_LOG(debug, "Received message from RDMA downstream: {}", message);
                
                bool pushed = upstream_to_downstream_buffer_->push(message);
                if (!pushed) {
                    ENVOY_LOG(error, "downstream_to_upstream_buffer_ is currently full");
                    if (!connection_close_) {
                        ENVOY_LOG(info, "Closed due to full downstream_to_upstream_buffer_");
                        close_procedure();
                    }
                    break;
                }

//                write(accessConnfd, (const void*) get_payload(ith), get_length(ith));
                cnt++;
                cur = std::chrono::high_resolution_clock::now();
	        }
		    //printf("%d\n", cnt);
	    };
        ENVOY_LOG(debug, "rdma_polling stopped");
    }

    int can_write(uint32_t offset, uint32_t serverPos, uint32_t margin) {
        if (offset<serverPos) {
            offset+=circle_size;
        }
        return (offset-serverPos) < margin;
    }

    volatile char *get_payload(volatile char *cur) {
        cur = cur + sizeof(uint32_t) + sizeof(char) + sizeof(uint32_t);
        return cur;
    }

    char *get_payload(char *cur) {
        cur = cur + sizeof(uint32_t) + sizeof(char) + sizeof(uint32_t);
        return cur;
    }
    
    // This function will run in a thread and be responsible for sending to upstream through RDMA
    void rdma_sender() {
        // RDMA utils
        char *curSegment;
        uint32_t offset = 0;
        uint32_t cnt = 0;
        infinity::requests::RequestToken requestToken(context);
        int hasRead = 0;
        int cntRead = 0;

        ENVOY_LOG(info, "rdma_sender launched");
        while (true) {
            if (!can_write(offset, *remotePosition, margin)) {
                cntRead++;
                qpToWrite->read(remoteMemory, remoteMemoryToken, sizeof(uint32_t), nullptr);
                hasRead=1;
                continue;
            }
            std::string item;
            if (downstream_to_upstream_buffer_->pop(item)) {
                ENVOY_LOG(debug, "Got item: {}", item);
                curSegment = get_ith(remoteHead, offset);
                memcpy(get_payload(curSegment), item.c_str(), item.size());

                set_toCheck(curSegment, '1');
		        uint32_t writeOffset = sizeof(uint32_t) + (segmentSize * offset);
		        set_length(curSegment, item.size());	
                
                uint32_t writeLength = 2*sizeof(uint32_t)+sizeof(char)+item.size();
                qpToWrite->write(remoteMemory, writeOffset, remoteMemoryToken, writeOffset, writeLength, infinity::queues::OperationFlags(), &requestToken);
                offset = (offset + 1) % circle_size;

                requestToken.waitUntilCompleted();
            }
            else { // No item was retrieved after 3 seconds
                if (!active_rdma_sender_) { // If timeout and flag false: stop thread
                    break;
                }
            }
        }
        ENVOY_LOG(info, "rdma_sender stopped");
    }

    // This function will run in a thread and be responsible sending responses to the client through the dispatcher
    void downstream_sender() {
        ENVOY_LOG(info, "downstream_sender launched");
        while (true) {
            std::string item;
            if (upstream_to_downstream_buffer_->pop(item)) {
                ENVOY_LOG(debug, "Got item: {}", item);
                
                // Use dispatcher and locking to ensure that the right thread executes the task (writing responses to the client)
                // Asynchronous task
                // TO TEST : put this in rdma polling instead
                auto weak_self = weak_from_this();
                dispatcher_->post([weak_self, buffer = std::make_shared<Buffer::OwnedImpl>(item)]() -> void {
                    if (auto self = weak_self.lock()) {
                        self->write_callbacks_->injectWriteDataToFilterChain(*buffer, false); // Inject data to the listener
                    }
                });
            }
            else { // No item was retrieved after 3 seconds
                if (!active_downstream_sender_) { // If timeout and flag false: stop thread
                    break;
                }
            }
        }
        ENVOY_LOG(info, "downstream_sender stopped");
    }

    // Handling termination of threads and close filter connections
    void close_procedure() {
        connection_close_ = true;
        
        // Flag set to false to stop active threads
        active_rdma_polling_ = false;
        active_rdma_sender_ = false;
        active_downstream_sender_ = false;

        // Wait for all threads to finish
        if (rdma_polling_thread_.joinable()) {
            rdma_polling_thread_.join();
        }
        if (rdma_sender_thread_.joinable()) {
            rdma_sender_thread_.join();
        }
        if (downstream_sender_thread_.joinable()) {
            downstream_sender_thread_.join();
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

        // Get an element from the buffer and return true if successful, false if it did not pop any item after 3 seconds
        bool pop(T& item) {
            std::unique_lock<std::mutex> lock(mutex_);
            if (size_ == 0) {
                if (!cv_.wait_for(lock, std::chrono::seconds(10), [this] { return size_ > 0; })) {
                    return false; // buffer is still empty after 3 seconds
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
    Network::ReadFilterCallbacks* read_callbacks_{}; // ReadFilter callback (handle data from downstream)
    Network::WriteFilterCallbacks* write_callbacks_{}; // WriterFilter callback (handle data from upstream)

    std::atomic<bool> connection_init_{true}; // Keep track of connection initialization (first message from client)
    std::atomic<bool> connection_close_{false}; // Keep track of connection state
    int sock_rdma_;
    int sock_distant_rdma_; // RDMA socket to communicate with upstream RDMA

    // RDMA stuff
    infinity::core::Context *context;
    infinity::queues::QueuePairFactory *qpFactory;
    infinity::queues::QueuePair *qpToPoll;
    infinity::queues::QueuePair *qpToWrite;
    const char* server_ip;
    uint32_t port_number;
    uint32_t circle_size;
    const uint32_t payloadBound = 1500;
    uint32_t segmentSize;
    uint32_t bufferSize;
    infinity::memory::RegionToken *remoteMemoryToken;
    infinity::memory::Buffer *remoteMemory;
    char *remoteBuffer;    
    char *remoteHead;
    uint32_t *remotePosition;
    uint32_t margin;
    uint32_t offset;
    infinity::memory::Buffer *hostMemory;
	infinity::memory::RegionToken *hostMemoryToken;
	volatile char *hostBuffer;
	volatile uint32_t *hostOffset;	
	volatile char *hostHead;


    std::shared_ptr<CircularBuffer<std::string>> downstream_to_upstream_buffer_ = std::make_shared<CircularBuffer<std::string>>(8388608); // Buffer supplied by onData() and consumed by the RDMA sender thread
    std::shared_ptr<CircularBuffer<std::string>> upstream_to_downstream_buffer_ = std::make_shared<CircularBuffer<std::string>>(8388608); // Buffer supplied by RDMA polling thread and consumed by downstream sender thread

    std::atomic<bool> active_rdma_polling_{true}; // If false, stop the thread
    std::atomic<bool> active_rdma_sender_{true}; // If false, stop the thread
    std::atomic<bool> active_downstream_sender_{true}; // If false, stop the thread

    std::thread rdma_polling_thread_;
    std::thread rdma_sender_thread_;
    std::thread downstream_sender_thread_;
    std::thread test_rdma_thread_;

    Envoy::Event::Dispatcher* dispatcher_{}; //  Used to give the control back to the thread responsible for writing responses to the client (used in downstream_sender())
};

} // namespace SenderRDMA
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
