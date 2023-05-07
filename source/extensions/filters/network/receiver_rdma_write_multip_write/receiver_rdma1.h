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
namespace ReceiverRDMA {

/**
 * Implementation of a custom RDMA receiver filter
 */
class ReceiverRDMAFilter : public Network::ReadFilter,
                       public Network::WriteFilter,
                       public Network::ConnectionCallbacks,
                       public std::enable_shared_from_this<ReceiverRDMAFilter>,
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
        if (event == Network::ConnectionEvent::RemoteClose ||
            event == Network::ConnectionEvent::LocalClose) {
                if (read_callbacks_->connection().state() == Network::Connection::State::Closed ||
                    read_callbacks_->connection().state() == Network::Connection::State::Closing) {
                        ENVOY_LOG(info, "read_callbacks_ CLOSED");
                        if (!connection_close_) {
                            ENVOY_LOG(info, "Close in read_callbacks_ event");
                            close_procedure();
                        }
                }
                if (write_callbacks_->connection().state() == Network::Connection::State::Closed ||
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
    ~ReceiverRDMAFilter() {
        ENVOY_LOG(info, "DESTRUCTOR");
        delete qpFactory;
	    delete context;	    
        delete qpToPoll;
        delete qpToWrite;
        delete remoteMemory;
        delete hostMemory;
        delete remoteMemoryToken;
        delete hostMemoryToken;
    }

    // Constructor
    ReceiverRDMAFilter() {
        ENVOY_LOG(info, "CONSTRUCTOR CALLED");
        // test_rdma_thread_ = std::thread(&ReceiverRDMAFilter::test_rdma, this);
    }

    void set_length(volatile char *cur, uint32_t length) {
        uint32_t *ptr = (uint32_t *) (cur + sizeof(uint32_t) + sizeof(char));
        *ptr = htonl(length);
    }

    volatile char *get_ith(volatile char *head, uint32_t i) {
        volatile char *ith = head+i*segmentSize;
        return ith;
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

        // Get source IP and source Port of read_callbacks connection
        Network::Connection& connection = read_callbacks_->connection();
        const auto& stream_info = connection.streamInfo();
        Network::Address::InstanceConstSharedPtr remote_address = stream_info.downstreamAddressProvider().remoteAddress();
        std::string source_ip = remote_address->ip()->addressAsString();
        uint32_t source_port = remote_address->ip()->port();
        ENVOY_LOG(debug, "Source IP: {}, Source Port {}", source_ip, source_port);

        // Create server RDMA socket: listen own IP, source Port+1
        circle_size = 100;
        port_number = source_port+1;
        segmentSize = 2*sizeof(uint32_t)+sizeof(char)+payloadBound;
        bufferSize = (circle_size * segmentSize )+sizeof(uint32_t);
        context = new infinity::core::Context();
        qpFactory = new infinity::queues::QueuePairFactory(context);
        

		hostMemory = new infinity::memory::Buffer(context, bufferSize); // todo : one more case for reader head
        ENVOY_LOG(debug, "BEFORE HOST MEMORY CREATE REGION TOKEN");
		hostMemoryToken = hostMemory->createRegionToken();
        ENVOY_LOG(debug, "BEFORE HOST MEMORY GET DATA");
		hostBuffer = (char *) hostMemory->getData();
		hostOffset = (uint32_t *) hostBuffer;
		*hostOffset = 0;
		hostHead = hostBuffer+sizeof(uint32_t); // first bytes to store cur position
		//memset(head, '0', CIRCLE_SIZE * payloadBound * sizeof(char));
		//printf("Setting up connection I think (blocking)\n"); 	
        ENVOY_LOG(debug, "BEFORE LOOP");	
		for (uint32_t i = 1; i<=circle_size; i++) {
 			//printf("cur : ");
			volatile char *ith = get_ith(hostHead, i);
			set_toCheck(ith, '0');
            ENVOY_LOG(debug, "AFTER SET_TOCHECK");
			//printf("%d ", i);
		}
        ENVOY_LOG(debug, "PORT: {}", port_number);
		qpFactory->bindToPort(port_number);
        ENVOY_LOG(debug, "BEFORE ACCEPT");
		qpToPoll = qpFactory->acceptIncomingConnection(hostMemoryToken, sizeof(infinity::memory::RegionToken)); // todo : retrieve 4-tuple
        ENVOY_LOG(debug, "CONNECTION RDMA ACCEPTED br");

        rdma_polling_thread_ = std::thread(&ReceiverRDMAFilter::rdma_polling, this);

        // is source_ip the ip of the other proxy ?
        qpToWrite = qpFactory->connectToRemoteHost(source_ip.c_str(), port_number);
		remoteMemoryToken = (infinity::memory::RegionToken *) qpToWrite->getUserData();
		////printf("Creating buffers\n");
		remoteMemory = new infinity::memory::Buffer(context, bufferSize);
		remoteBuffer = (char *) remoteMemory->getData();
		remoteHead = remoteBuffer + sizeof(uint32_t);
		remotePosition = (uint32_t *) remoteBuffer;
		margin = (circle_size/2)-1;
		remoteOffset = 0;	
        ENVOY_LOG(debug, "CONNECTION RDMA 2 ACCEPTED");

        rdma_sender_thread_ = std::thread(&ReceiverRDMAFilter::rdma_sender, this);
        // Accepts connection from downstream RDMA 
        upstream_sender_thread_ = std::thread(&ReceiverRDMAFilter::upstream_sender, this);
        // Server RDMA socket connects to downstream client RDMA socket with (destination_ip, source_port+1)
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
    
    char get_toCheck(volatile char *cur) {
        cur = cur + sizeof(uint32_t);
        return *cur;
    }

    uint32_t get_length(volatile char *cur) {
        cur = cur + sizeof(uint32_t) + sizeof(char);
        return ntohl(*((uint32_t *)cur));
    }

    // This function will run in a thread and be responsible for RDMA polling
    void rdma_polling() {
        ENVOY_LOG(debug, "rdma_polling launched");

        /*const int BUFFER_SIZE = 1024;
        char buffer[BUFFER_SIZE];
        int bytes_received;

        struct pollfd poll_fds[1];
        poll_fds[0].fd = sock_rdma_;
        poll_fds[0].events = POLLIN;

        while (true) {
            // Poll data
            int ret = poll(poll_fds, 1, 3000); // Timeout after 3 seconds if no received data

            if (ret < 0) {
                ENVOY_LOG(error, "poll error");
                if (!connection_close_) {
                    ENVOY_LOG(info, "Closed due to poll error");
                    close_procedure();
                }
                break;
            }

            // If timeout and flag false: stop thread
            else if (ret == 0 && !active_rdma_polling_) {
                break;
            }

            // Receive data on the socket
            else if (poll_fds[0].revents & POLLIN) {
                memset(buffer, '\0', BUFFER_SIZE);
                bytes_received = recv(sock_rdma_, buffer, BUFFER_SIZE, 0);
                if (bytes_received < 0) {
                    ENVOY_LOG(debug, "Error receiving message from RDMA downstream");
                    break;
                } 
                else if (bytes_received == 0) {
                    ENVOY_LOG(debug, "RDMA downstream closed the connection");
                    break;
                }
                std::string message(buffer, bytes_received); // Put the received data in a string
                ENVOY_LOG(debug, "Received message from RDMA downstream: {}", message);

                // Push the message for the upstream
                bool pushed = downstream_to_upstream_buffer_->push(message);
                if (!pushed) {
                    ENVOY_LOG(error, "downstream_to_upstream_buffer_ is currently full");
                    if (!connection_close_) {
                        ENVOY_LOG(info, "Closed due to full downstream_to_upstream_buffer_");
                        close_procedure();
                    }
                    break;
                }
            }
        }*/
        uint32_t actualOffset = 0;		
	    uint32_t writtenOffset = 0;
	    int cnt = 0;
        
        // cur clock = clock();
        auto cur = std::chrono::high_resolution_clock::now();
	    while (1) {	
            if (!active_rdma_polling_) {
                ENVOY_LOG(debug, "break0");
                break;
            }
            volatile char *ith = get_ith(hostHead, actualOffset);

            auto ms = std::chrono::duration_cast<std::chrono::seconds>(cur.time_since_epoch()).count();
            if (ms>6000) {
                if (!active_rdma_polling_) {
                    ENVOY_LOG(debug, "break");
                    break;
                }
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

                bool pushed = downstream_to_upstream_buffer_->push(message);
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

    // This function will run in a thread and be responsible for sending to downstream through RDMA
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
            std::string item;
            if (upstream_to_downstream_buffer_->pop(item)) {
                ENVOY_LOG(debug, "Got item: {}", item);

                if (!can_write(offset, *remotePosition, margin)) {
                    cntRead++;
                    qpToWrite->read(remoteMemory, remoteMemoryToken, sizeof(uint32_t), nullptr);
                    hasRead=1;
                    continue;
                }

                curSegment = get_ith(remoteHead, offset);
                memcpy(get_payload(curSegment), item.c_str(), item.size());
                ENVOY_LOG(debug, "item size: {}", item.size());

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


    // This function will run in a thread and be responsible for sending requests to the server through the dispatcher
    void upstream_sender() {
        // refactor ?
        // int index = 0; while(1) if (get_toCheck(index)...) index++
        ENVOY_LOG(info, "upstream_sender launched");
        while (true) {
            std::string item;
            if (downstream_to_upstream_buffer_->pop(item)) {
                ENVOY_LOG(debug, "Got item: {}", item);
                ENVOY_LOG(debug, "item size: {}", item.size());

                // Use dispatcher and locking to ensure that the right thread executes the task (sending requests to the server)
                // Asynchronous task

                // to test : put it in the polling rdma instead
                auto weak_self = weak_from_this();
                dispatcher_->post([weak_self, buffer = std::make_shared<Buffer::OwnedImpl>(item)]() -> void {
                    if (auto self = weak_self.lock()) {
                        self->read_callbacks_->injectReadDataToFilterChain(*buffer, false); // Inject data to tcp_proxy
                    }
                });
            }
            else { // No item was retrieved after 3 seconds
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
    int sock_rdma_; // RDMA socket to communicate with downstream RDMA
    uint32_t circle_size;
    uint32_t port_number;
    const uint32_t payloadBound = 1500;
    uint32_t segmentSize;
    uint32_t bufferSize;
    infinity::core::Context *context;
    infinity::queues::QueuePairFactory *qpFactory;
    infinity::queues::QueuePair *qpToPoll;
    infinity::memory::Buffer *hostMemory; // todo : one more case for reader head
    infinity::memory::RegionToken *hostMemoryToken;
    volatile char *hostBuffer;
    volatile uint32_t *hostOffset;
    volatile char *hostHead; 
    infinity::queues::QueuePair *qpToWrite;
    infinity::memory::RegionToken *remoteMemoryToken;
	infinity::memory::Buffer *remoteMemory;
	char *remoteBuffer;
    char *remoteHead;
    uint32_t *remotePosition;
    uint32_t margin;
    uint32_t remoteOffset;	



    std::shared_ptr<CircularBuffer<std::string>> downstream_to_upstream_buffer_ = std::make_shared<CircularBuffer<std::string>>(8388608); // Buffer supplied by RDMA polling thread and consumed by the upstream sender thread
    std::shared_ptr<CircularBuffer<std::string>> upstream_to_downstream_buffer_ = std::make_shared<CircularBuffer<std::string>>(8388608); // Buffer supplied by onWrite() and consumed by RDMA sender thread

    Envoy::Event::Dispatcher* dispatcher_{}; // Used to give the control back to the thread responsible for sending requests to the server (used in upstream_sender())

    std::thread rdma_polling_thread_;
    std::thread rdma_sender_thread_;
    std::thread upstream_sender_thread_;
    std::thread test_rdma_thread_;

    std::atomic<bool> active_rdma_polling_{true}; // If false, stop the thread
    std::atomic<bool> active_rdma_sender_{true}; // If false, stop the thread
    std::atomic<bool> active_upstream_sender_{true}; // If false, stop the thread
};

} // namespace ReceiverRDMA
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
