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
    }

    // Constructor
    SenderRDMAFilter() {
        ENVOY_LOG(info, "CONSTRUCTOR CALLED");
        // test_rdma_thread_ = std::thread(&SenderRDMAFilter::test_rdma, this);
    }

    void setup_rdma() {
        ENVOY_LOG(debug, "launch setup_rdma");
        
        Network::Connection& connection = write_callbacks_->connection();
        uint32_t sourcePort = write_callbacks_->connection().streamInfo().upstreamInfo().get()->upstreamLocalAddress()->ip()->port();
        std::string destinationIp = write_callbacks_->connection().streamInfo().upstreamInfo().get()->upstreamRemoteAddress()->ip()->addressAsString();
        ENVOY_LOG(debug, "Destination IP: {}, Source Port {}", destinationIp, sourcePort);                
        serverIp = destinationIp.c_str();
        serverPort = sourcePort+1;        
        ENVOY_LOG(debug, "IP: {}", serverIp);
        ENVOY_LOG(debug, "PORT: {}", serverPort);

        context = new infinity::core::Context();
        qpFactory = new infinity::queues::QueuePairFactory(context);
        
        qp = qpFactory->connectToRemoteHost(serverIp, serverPort);

        // client -> server
		sendBuffer = new infinity::memory::Buffer(context, payloadBound * sizeof(char));        
		receiveBuffer = new infinity::memory::Buffer(context, sizeof(char));
        
        // server-client
        receiveBuffers = new infinity::memory::Buffer *[circleSize];
		for (uint32_t i = 0; i < circleSize; ++i) {
			receiveBuffers[i] = new infinity::memory::Buffer(context, payloadBound * sizeof(char));
			context->postReceiveBuffer(receiveBuffers[i]);
		}
        
        rdma_sender_thread_ = std::thread(&SenderRDMAFilter::rdma_sender, this);
        rdma_polling_thread_ = std::thread(&SenderRDMAFilter::rdma_polling, this);
        downstream_sender_thread_ = std::thread(&SenderRDMAFilter::downstream_sender, this);
    }

    void rdma_polling() {
        //std::vector<std::string> myLogs;
        ENVOY_LOG(info, "rdma_polling started");
        while (1) {
            int cnt = 0;
            while (!context->receive(&receiveElement)) {
                if (++cnt>1000000000) {   
                    if (!active_rdma_polling_) {
                        ENVOY_LOG(info, "rdma_polling stopped");
                        return;
                    }                                                                    
                }
            }          
            std::string message((char *) receiveElement.buffer->getData(), receiveElement.bytesWritten); // Put the received data in a string                
                
            bool pushed = upstream_to_downstream_buffer_->push(message);
            if (!pushed) {
                ENVOY_LOG(error, "upstream_to_downstream_buffer_ is currently full");
                if (!connection_close_) {
                    ENVOY_LOG(info, "Closed due to full upstream_to_downstream_buffer_");
                    close_procedure();
                }
                break;
            }

            context->postReceiveBuffer(receiveElement.buffer);
        }    
    }
    
    // This function will run in a thread and be responsible for sending to upstream through RDMA
    void rdma_sender() {
        ENVOY_LOG(info, "rdma_sender launched");
        while (1) {    
            std::string item;    
            if (downstream_to_upstream_buffer_->pop(item)) {     // to opti           
                infinity::requests::RequestToken requestToken(context);
                memcpy(sendBuffer->getData(), item.c_str(), item.size());
                qp->send(sendBuffer, item.size(), &requestToken);
                requestToken.waitUntilCompleted();    
            } else { // No item was retrieved after 3 seconds
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
                ENVOY_LOG(debug, "item size: {}", item.size());
                
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

    // RDMA stuff    
    uint32_t payloadBound = 1500;
    uint32_t circleSize = 100;
    const char* serverIp;
    uint32_t portNumber;
    uint32_t serverPort;

    infinity::core::Context *context;
    infinity::queues::QueuePairFactory *qpFactory;
    infinity::queues::QueuePair *qp;
    infinity::memory::Buffer **receiveBuffers;
    infinity::core::receive_element_t receiveElement;
    infinity::memory::Buffer *sendBuffer;
    infinity::memory::Buffer *receiveBuffer;


    std::shared_ptr<CircularBuffer<std::string>> downstream_to_upstream_buffer_ = std::make_shared<CircularBuffer<std::string>>(8388608); // Buffer supplied by onData() and consumed by the RDMA sender thread
    std::shared_ptr<CircularBuffer<std::string>> upstream_to_downstream_buffer_ = std::make_shared<CircularBuffer<std::string>>(8388608); // Buffer supplied by RDMA polling thread and consumed by downstream sender thread

    std::atomic<bool> active_rdma_polling_{true}; // If false, stop the thread
    std::atomic<bool> active_rdma_sender_{true}; // If false, stop the thread
    std::atomic<bool> active_downstream_sender_{true}; // If false, stop the thread

    std::thread rdma_polling_thread_;
    std::thread rdma_sender_thread_;
    std::thread downstream_sender_thread_;
    std::thread setup_rdma_thread_;

    Envoy::Event::Dispatcher* dispatcher_{}; //  Used to give the control back to the thread responsible for writing responses to the client (used in downstream_sender())
};

} // namespace SenderRDMA
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
