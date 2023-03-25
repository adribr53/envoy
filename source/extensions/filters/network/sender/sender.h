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

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace Sender {

/**
 * Implementation of a custom sender filter
 */
class SenderFilter : public Network::ReadFilter,
                     public Network::WriteFilter,
                     public Network::ConnectionCallbacks,
                     public std::enable_shared_from_this<SenderFilter>,
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
    ~SenderFilter() {
        ENVOY_LOG(info, "DESTRUCTOR");
        close(sock_rdma_);
        close(sock_distant_rdma_);
    }

    // Constructor
    SenderFilter(Envoy::Thread::ThreadFactory& thread_factory)
        : thread_factory_(thread_factory) {
        ENVOY_LOG(info, "CONSTRUCTOR CALLED");
    }

    // This function will run in a thread and be responsible for RDMA polling
    void rdma_polling() {
        ENVOY_LOG(info, "rdma_polling launched");

        // Wait and accept incoming RDMA connection
        listen(sock_rdma_, 1);
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        ENVOY_LOG(info, "Waiting for incoming RDMA connection...");
        // sock_distant_rdma_ is used to communicate with upstream RDMA
        sock_distant_rdma_ = accept(sock_rdma_, reinterpret_cast<struct sockaddr*>(&client_addr), &client_addr_len);
        ENVOY_LOG(info, "RDMA Connection from upstream proxy accepted");

        // Launch RDMA sender thread
        rdma_sender_thread_ = thread_factory_.createThread([this]() {this->rdma_sender();}, absl::nullopt);

        // Launch downstream sender thread
        downstream_sender_thread_ = thread_factory_.createThread([this]() {this->downstream_sender();}, absl::nullopt);

        // Start polling
        const int BUFFER_SIZE = 1024;
        char buffer[BUFFER_SIZE];
        int bytes_received;

        struct pollfd poll_fds[1];
        poll_fds[0].fd = sock_distant_rdma_;
        poll_fds[0].events = POLLIN;

        while (active_rdma_polling_) {
            // Poll data
            int ret = poll(poll_fds, 1, 0);

            if (ret < 0) {
                ENVOY_LOG(error, "Poll error");
            }

            // If timeout
            else if (ret == 0) {
            }

            // Receive data on the socket
            else if (poll_fds[0].revents & POLLIN) {
                memset(buffer, '\0', BUFFER_SIZE);
                bytes_received = recv(sock_distant_rdma_, buffer, BUFFER_SIZE, 0);
                if (bytes_received < 0) {
                    ENVOY_LOG(debug, "Error receiving message from RDMA upstream");
                    continue;
                } 
                else if (bytes_received == 0) {
                    ENVOY_LOG(debug, "No bytes received from RDMA Upstream");
                    continue;
                }
                std::string message(buffer, bytes_received); // Put the received data in a string
                ENVOY_LOG(debug, "Received message from RDMA upstream: {}", message);
                push(upstream_to_downstream_buffer_, message); // Push received data in circular buffer
            }
        }
        ENVOY_LOG(info, "rdma_polling stopped");
    }

    // This function will run in a thread and be responsible for sending to upstream through RDMA
    void rdma_sender() {
        ENVOY_LOG(info, "rdma_sender launched");
        while (active_rdma_sender_) {
            std::string item;
            if (downstream_to_upstream_buffer_->pop(item)) {
                ENVOY_LOG(debug, "Got item: {}", item);
                send(sock_distant_rdma_, item.c_str(), size(item), 0);
            }
        }
        ENVOY_LOG(info, "rdma_sender stopped");
    }

    // This function will run in a thread and be responsible sending responses to the client through the dispatcher
    void downstream_sender() {
        ENVOY_LOG(info, "downstream_sender launched");
        while (active_downstream_sender_) {
            std::string item;
            if (upstream_to_downstream_buffer_->pop(item)) {
                ENVOY_LOG(debug, "Got item: {}", item);
                
                // Use dispatcher and locking to ensure that the right thread executes the task (writing responses to the client)
                // Asynchronous task
                auto weak_self = weak_from_this();
                dispatcher_->post([weak_self, buffer = std::make_shared<Buffer::OwnedImpl>(item)]() -> void {
                    if (auto self = weak_self.lock()) {
                        self->write_callbacks_->injectWriteDataToFilterChain(*buffer, false); // Inject data to the listener
                    }
                });
            }
        }
        ENVOY_LOG(info, "downstream_sender stopped");
    }

    // Handling termination of threads and close filter connections
    void close_procedure() {
        // Flag set to false to stop active threads
        active_rdma_polling_ = false;
        active_rdma_sender_ = false;
        active_downstream_sender_ = false;

        // Wait for all threads to finish
        if (rdma_polling_thread_ != nullptr) {
            rdma_polling_thread_.get()->join();
            rdma_polling_thread_ = nullptr;
        }
        if (rdma_sender_thread_ != nullptr) {
            rdma_sender_thread_.get()->join();
            rdma_sender_thread_ = nullptr;
        }
        if (downstream_sender_thread_!= nullptr) {
            downstream_sender_thread_.get()->join();
            downstream_sender_thread_ = nullptr;
        }
        ENVOY_LOG(info, "All threads terminated");

        // Close filter connections
        connection_close_ = true;
        if (read_callbacks_->connection().state() == Network::Connection::State::Open) {
            read_callbacks_->connection().close(Network::ConnectionCloseType::Abort);
        }
        if (write_callbacks_->connection().state() == Network::Connection::State::Open) {
            write_callbacks_->connection().close(Network::ConnectionCloseType::Abort);
        }
    }

    // Thread-safe & non-blocking Circular buffer
    // 2 operations: push() and pop() an item
    #include <atomic>
    #include <memory>
    #include <cassert>

    template<typename T>
    class CircularBuffer
    {
    public:
        explicit CircularBuffer(std::size_t capacity)
            : data_(std::make_unique<T[]>(capacity))
            , capacity_(capacity)
        {
            assert(capacity > 0);
        }

        CircularBuffer(const CircularBuffer&) = delete;
        CircularBuffer& operator=(const CircularBuffer&) = delete;

        bool push(const T& value)
        {
            const auto current_tail = tail_.load(std::memory_order_relaxed);
            const auto next_tail = increment(current_tail);
            if (next_tail != head_.load(std::memory_order_acquire))
            {
                data_[current_tail] = value;
                tail_.store(next_tail, std::memory_order_release);
                return true;
            }
            return false;
        }

        bool pop(T& value)
        {
            const auto current_head = head_.load(std::memory_order_relaxed);
            if (current_head == tail_.load(std::memory_order_acquire))
            {
                return false;
            }
            value = data_[current_head];
            head_.store(increment(current_head), std::memory_order_release);
            return true;
        }

    private:
        std::unique_ptr<T[]> data_;
        const std::size_t capacity_;
        std::atomic<std::size_t> head_{0};
        std::atomic<std::size_t> tail_{0};

        std::size_t increment(std::size_t idx) const noexcept
        {
            return (idx + 1) % capacity_;
        }
    };

    // Push a data string in a specified circular buffer
    // Keep trying to push until there is an available space in the buffer
    void push(std::shared_ptr<CircularBuffer<std::string>> buffer, std::string dataStr) {
        bool pushed = false;
        while (!pushed) {
            pushed = buffer->push(dataStr);
            if (!pushed) {
                ENVOY_LOG(info, "Circular buffer is currently full");
            }
        }
    }

private:
    Network::ReadFilterCallbacks* read_callbacks_{}; // ReadFilter callback (handle data from downstream)
    Network::WriteFilterCallbacks* write_callbacks_{}; // WriterFilter callback (handle data from upstream)

    bool connection_init_{true}; // Keep track of connection initialization (first message from client)
    bool connection_close_{false}; // Keep track of connection state
    int sock_rdma_;
    int sock_distant_rdma_; // RDMA socket to communicate with upstream RDMA

    std::shared_ptr<CircularBuffer<std::string>> downstream_to_upstream_buffer_ = std::make_shared<CircularBuffer<std::string>>(4096); // Buffer supplied by onData() and consumed by the RDMA sender thread
    std::shared_ptr<CircularBuffer<std::string>> upstream_to_downstream_buffer_ = std::make_shared<CircularBuffer<std::string>>(4096); // Buffer supplied by RDMA polling thread and consumed by downstream sender thread

    std::atomic<bool> active_rdma_polling_{true}; // If false, stop the thread
    std::atomic<bool> active_rdma_sender_{true}; // If false, stop the thread
    std::atomic<bool> active_downstream_sender_{true}; // If false, stop the thread

    Envoy::Thread::ThreadPtr rdma_polling_thread_;
    Envoy::Thread::ThreadPtr rdma_sender_thread_;
    Envoy::Thread::ThreadPtr downstream_sender_thread_;

    Envoy::Event::Dispatcher* dispatcher_{}; //  Used to give the control back to the thread responsible for writing responses to the client (used in downstream_sender())
    Envoy::Thread::ThreadFactory& thread_factory_; // Used to create the threads (with Envoy API)
};

} // namespace Sender
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
