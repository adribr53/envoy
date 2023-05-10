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

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace Receiver {

/**
 * Implementation of a custom TCP receiver filter
 */
class ReceiverFilter : public Network::ReadFilter,
                       public Network::WriteFilter,
                       public Network::ConnectionCallbacks,
                       public std::enable_shared_from_this<ReceiverFilter>,
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
    ~ReceiverFilter() {
        ENVOY_LOG(info, "DESTRUCTOR");
        close(sock_tcp_);
    }

    // Constructor
    ReceiverFilter(const uint32_t payloadBound, const uint32_t circleSize, const uint32_t timeToWrite, const uint32_t sharedBufferSize)
        : payloadBound_(payloadBound), circleSize_(circleSize), timeToWrite_(timeToWrite), sharedBufferSize_(sharedBufferSize)
    {
        ENVOY_LOG(info, "CONSTRUCTOR CALLED");
        ENVOY_LOG(info, "payloadBound_: {}", payloadBound_);
        ENVOY_LOG(info, "circleSize_: {}", circleSize_);
        ENVOY_LOG(info, "timeToWrite_: {}", timeToWrite_);
        ENVOY_LOG(info, "sharedBufferSize_: {}", sharedBufferSize_);

        downstream_to_upstream_buffer_ = std::make_shared<CircularBuffer<std::string>>(sharedBufferSize_);
        upstream_to_downstream_buffer_ = std::make_shared<CircularBuffer<std::string>>(sharedBufferSize_);
    }

    // This function is responsible for initializing the TCP conneciton in both directions
    void tcp_setup() {
        ENVOY_LOG(debug, "launch tcp_setup");

        // Get source IP and source Port of read_callbacks connection
        Network::Connection& connection = read_callbacks_->connection();
        const auto& stream_info = connection.streamInfo();
        Network::Address::InstanceConstSharedPtr remote_address = stream_info.downstreamAddressProvider().remoteAddress();
        std::string source_ip = remote_address->ip()->addressAsString();
        uint32_t source_port = remote_address->ip()->port();
        ENVOY_LOG(debug, "Source IP: {}, Source Port {}", source_ip, source_port);

        // Connect to TCP downstream using source port+1
        struct sockaddr_in downstream_address_;
        uint32_t downstream_port = source_port+1;
        downstream_address_.sin_family = AF_INET;
        downstream_address_.sin_port = htons(downstream_port);
        ENVOY_LOG(debug, "DOWNSTREAM IP: {}, Port: {}", source_ip, downstream_port);

        if (inet_pton(AF_INET, source_ip.c_str(), &downstream_address_.sin_addr) < 0) {
            ENVOY_LOG(error, "error inet_pton");
            if (!connection_close_) {
                ENVOY_LOG(info, "Closed due to inet_pton");
                close_procedure();
            }
            return;
        }

        // Try to connect to TCP downstream (try for 10 seconds)
        int count = 0;
        sock_tcp_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_tcp_ < 0) {
            ENVOY_LOG(error, "error creating sock_tcp_");
            if (!connection_close_) {
                ENVOY_LOG(info, "Closed due to error creating sock_tcp_");
                close_procedure();
            }
            return;
        }

        while ((connect(sock_tcp_, reinterpret_cast<struct sockaddr*>(&downstream_address_), sizeof(downstream_address_ ))) != 0) {
            if (count >= 10) {
                ENVOY_LOG(error, "TCP failed to connect to downstream");
                if (!connection_close_) {
                    ENVOY_LOG(info, "Closed due to TCP failed to connect to downstream");
                    close_procedure();
                }
                return;
            }
            ENVOY_LOG(debug, "RETRY CONNECTING TO TCP DOWNSTREAM...");
            sleep(1);
            count++;
        }
        ENVOY_LOG(debug, "CONNECTED TO TCP DOWNSTREAM");

        // Launch TCP polling thread
        tcp_polling_thread_ = std::thread(&ReceiverFilter::tcp_polling, this);

        // Launch TCP sender thread
        tcp_sender_thread_ = std::thread(&ReceiverFilter::tcp_sender, this);

        // Launch upstream sender thread
        upstream_sender_thread_ = std::thread(&ReceiverFilter::upstream_sender, this);
    }

    // This function will run in a thread and be responsible for TCP polling
    void tcp_polling() {
        ENVOY_LOG(info, "tcp_polling launched");

        const int BUFFER_SIZE = payloadBound_;
        char buffer[BUFFER_SIZE];
        int bytes_received;

        struct pollfd poll_fds[1];
        poll_fds[0].fd = sock_tcp_;
        poll_fds[0].events = POLLIN;

        while (true) {
            // Poll data
            int ret = poll(poll_fds, 1, timeout_value_ * 1000); // Timeout after timeout_value_ seconds if no received data

            if (ret < 0) {
                ENVOY_LOG(error, "poll error");
                if (!connection_close_) {
                    ENVOY_LOG(info, "Closed due to poll error");
                    close_procedure();
                }
                break;
            }

            // If timeout and flag false: stop thread
            else if (ret == 0 && !active_tcp_polling_) {
                break;
            }

            // Receive data on the socket
            else if (poll_fds[0].revents & POLLIN) {
                memset(buffer, '\0', BUFFER_SIZE);
                bytes_received = recv(sock_tcp_, buffer, BUFFER_SIZE, 0);
                if (bytes_received < 0) {
                    ENVOY_LOG(debug, "Error receiving message from TCP downstream");
                    break;
                } 
                else if (bytes_received == 0) {
                    ENVOY_LOG(debug, "TCP downstream closed the connection");
                    break;
                }
                std::string message(buffer, bytes_received); // Put the received data in a string
                ENVOY_LOG(debug, "Received message from TCP downstream: {}", message);

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
        }
        ENVOY_LOG(info, "tcp_polling stopped");
    }

    // This function will run in a thread and be responsible for sending to downstream through TCP
    void tcp_sender() {
        ENVOY_LOG(info, "tcp_sender launched");
        while (true) {
            std::string item;
            if (upstream_to_downstream_buffer_->pop(item)) {
                ENVOY_LOG(debug, "Got item: {}", item);
                if (send(sock_tcp_, item.c_str(), size(item), 0) < 0) {
                    ENVOY_LOG(debug, "Error sending TCP message");
                    continue;
                }
            }
            else { // No item was retrieved after timeout_value_ seconds
                if (!active_tcp_sender_) { // If timeout and flag false: stop thread
                    break;
                }
            }
        }
        ENVOY_LOG(info, "tcp_sender stopped");
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
        active_tcp_polling_ = false;
        active_tcp_sender_ = false;
        active_upstream_sender_ = false;

        // Wait for all threads to finish
        if (tcp_polling_thread_.joinable()) {
            tcp_polling_thread_.join();
        }
        if (tcp_sender_thread_.joinable()) {
            tcp_sender_thread_.join();
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

    // Connection flag
    std::atomic<bool> connection_close_{false}; // Keep track of connection state

    // Socket
    int sock_tcp_; // TCP socket to communicate with downstream TCP

    // Timeout
    const static uint32_t timeout_value_ = 1; // In seconds

    // Chunk size
    const uint64_t payloadBound_;
    const uint32_t circleSize_;
    const uint32_t timeToWrite_;
    const uint32_t sharedBufferSize_;

    // Buffers
    std::shared_ptr<CircularBuffer<std::string>> downstream_to_upstream_buffer_; // Buffer supplied by TCP polling thread and consumed by the upstream sender thread
    std::shared_ptr<CircularBuffer<std::string>> upstream_to_downstream_buffer_; // Buffer supplied by onWrite() and consumed by TCP sender thread

    // Dispatcher
    Envoy::Event::Dispatcher* dispatcher_{}; // Used to give the control back to the thread responsible for sending requests to the server (used in upstream_sender())

    // Threads
    std::thread tcp_polling_thread_;
    std::thread tcp_sender_thread_;
    std::thread upstream_sender_thread_;

    // Thread flags
    std::atomic<bool> active_tcp_polling_{true}; // If false, stop the thread
    std::atomic<bool> active_tcp_sender_{true}; // If false, stop the thread
    std::atomic<bool> active_upstream_sender_{true}; // If false, stop the thread
};

} // namespace Receiver
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
