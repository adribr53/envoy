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
 * Implementation of a custom sender RDMA filter
 */
class SenderRDMAFilter : 
                     public Network::ReadFilter,
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
                if (read_callbacks_->connection().state() == Network::Connection::State::Closed || // Downstream connection closed
                    read_callbacks_->connection().state() == Network::Connection::State::Closing) {
                    ENVOY_LOG(info, "read_callbacks_ CLOSED");
                    if (!connection_close_) {
                        ENVOY_LOG(info, "Close in read_callbacks_ event");
                        // Terminate threads and close filter connections
                        close_procedure();
                    }
                }
                if (write_callbacks_->connection().state() == Network::Connection::State::Closed || // Upstream connection closed
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
        // Free resources
        delete context_;
        delete qpFactory_;
        delete qp_;
        delete sendBuffer_;
        for (uint32_t i = 0; i < circleSize_; ++i) {
			delete receiveBuffers_[i];
		}
        delete receiveBuffers_;
    }

    // Constructor
    SenderRDMAFilter() {
        ENVOY_LOG(info, "CONSTRUCTOR CALLED");
    }

    // This function is responsible for initializing the RDMA conneciton in both directions
    void setup_rdma() {
        ENVOY_LOG(debug, "launch setup_rdma");
        
        // Get destination IP and destination port of upstream connection
        std::string destinationIp = write_callbacks_->connection().streamInfo().upstreamInfo().get()->upstreamRemoteAddress()->ip()->addressAsString();
        uint32_t destinationPort = write_callbacks_->connection().streamInfo().upstreamInfo().get()->upstreamLocalAddress()->ip()->port();            
        const char* serverIp = destinationIp.c_str();
        uint32_t serverPort = destinationPort+1;        
        ENVOY_LOG(debug, "Destination IP: {}, Destination Port {}", serverIp, serverPort);   

        context_ = new infinity::core::Context();
        qpFactory_ = new infinity::queues::QueuePairFactory(context_);

        // Connect to upstream
        qp_ = qpFactory_->connectToRemoteHost(serverIp, serverPort);

		sendBuffer_ = new infinity::memory::Buffer(context_, payloadBound_ * sizeof(char));        
        receiveBuffers_ = new infinity::memory::Buffer *[circleSize_];
		for (uint32_t i = 0; i < circleSize_; ++i) {
			receiveBuffers_[i] = new infinity::memory::Buffer(context_, payloadBound_ * sizeof(char));
			context_->postReceiveBuffer(receiveBuffers_[i]);
		}
        
        rdma_sender_thread_ = std::thread(&SenderRDMAFilter::rdma_sender, this);
        rdma_polling_thread_ = std::thread(&SenderRDMAFilter::rdma_polling, this);
        downstream_sender_thread_ = std::thread(&SenderRDMAFilter::downstream_sender, this);
    }

    // This function will run in a thread and be responsible for RDMA polling
    void rdma_polling() {
        ENVOY_LOG(info, "rdma_polling started");
        while (true) {
            int cnt = 0;
            // RECV from upstream
            while (!context_->receive(&receiveElement_)) {
                if (++cnt > 1000000) { // Serves as a timeout
                    if (!active_rdma_polling_) {
                        ENVOY_LOG(info, "rdma_polling stopped");
                        return;
                    }                                                                    
                }
            }        

            std::string message((char *) receiveElement_.buffer->getData(), receiveElement_.bytesWritten); // Put the received data in a string                
            // Push the data in the circular buffer
            bool pushed = upstream_to_downstream_buffer_->push(message);
            if (!pushed) {
                ENVOY_LOG(error, "upstream_to_downstream_buffer_ is currently full");
                if (!connection_close_) {
                    ENVOY_LOG(info, "Closed due to full upstream_to_downstream_buffer_");
                    close_procedure();
                }
                break;
            }
            context_->postReceiveBuffer(receiveElement_.buffer);
        }
        ENVOY_LOG(info, "rdma_polling stopped");
    }
    
    // This function will run in a thread and be responsible for sending to upstream through RDMA
    void rdma_sender() {
        ENVOY_LOG(info, "rdma_sender launched");
        while (true) {    
            std::string item;
            if (downstream_to_upstream_buffer_->pop(item)) { // to opti   
                ENVOY_LOG(debug, "Got item: {}", item);        

                // SEND to upstream
                infinity::requests::RequestToken requestToken(context_);
                memcpy(sendBuffer_->getData(), item.c_str(), item.size());
                qp_->send(sendBuffer_, item.size(), &requestToken);
                requestToken.waitUntilCompleted();    
            }
            else { // No item was retrieved after timeout_value seconds
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

            else { // No item was retrieved after timeout_value seconds
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

        // Get an element from the buffer and return true if successful, false if it did not pop any item after timeout_value seconds
        bool pop(T& item) {
            std::unique_lock<std::mutex> lock(mutex_);
            if (size_ == 0) {
                if (!cv_.wait_for(lock, std::chrono::seconds(timeout_value_), [this] { return size_ > 0; })) {
                    return false; // buffer is still empty after timeout_value seconds
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
    std::atomic<bool> connection_init_{true}; // Keep track of connection initialization (first message from client)
    std::atomic<bool> connection_close_{false}; // Keep track of connection state

    // Timeout
    const static uint32_t timeout_value_ = 1; // In seconds

    // RDMA stuff
    const uint64_t payloadBound_ = 1500;
    const uint32_t circleSize_ = 100;
    infinity::core::Context *context_;
    infinity::queues::QueuePairFactory *qpFactory_;
    infinity::queues::QueuePair *qp_;
    infinity::memory::Buffer **receiveBuffers_;
    infinity::core::receive_element_t receiveElement_;
    infinity::memory::Buffer *sendBuffer_;

    // Buffers
    std::shared_ptr<CircularBuffer<std::string>> downstream_to_upstream_buffer_ = std::make_shared<CircularBuffer<std::string>>(8388608); // Buffer supplied by onData() and consumed by the RDMA sender thread
    std::shared_ptr<CircularBuffer<std::string>> upstream_to_downstream_buffer_ = std::make_shared<CircularBuffer<std::string>>(8388608); // Buffer supplied by RDMA polling thread and consumed by downstream sender thread

    // Thread flags
    std::atomic<bool> active_rdma_polling_{true}; // If false, stop the thread
    std::atomic<bool> active_rdma_sender_{true}; // If false, stop the thread
    std::atomic<bool> active_downstream_sender_{true}; // If false, stop the thread

    // Threads
    std::thread rdma_polling_thread_;
    std::thread rdma_sender_thread_;
    std::thread downstream_sender_thread_;

    // Dispatcher
    Envoy::Event::Dispatcher* dispatcher_{}; //  Used to give the control back to the thread responsible for writing responses to the client (used in downstream_sender())
};

} // namespace SenderRDMA
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
