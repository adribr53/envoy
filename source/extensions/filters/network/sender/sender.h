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
        read_callbacks_ = &callbacks;
        read_callbacks_->connection().addConnectionCallbacks(*this);
    }

    // Network::WriteFilter
    Network::FilterStatus onWrite(Buffer::Instance& data, bool end_stream) override;
    void initializeWriteFilterCallbacks(Network::WriteFilterCallbacks& callbacks) override {
        write_callbacks_ = &callbacks;
        dispatcher_ = &write_callbacks_->connection().dispatcher();
        write_callbacks_->connection().addConnectionCallbacks(*this);
    }

    // Events
    void onEvent(Network::ConnectionEvent event) override {
        if (event == Network::ConnectionEvent::RemoteClose ||
            event == Network::ConnectionEvent::LocalClose) {
                if (read_callbacks_->connection().state() == Network::Connection::State::Closed ||
                    read_callbacks_->connection().state() == Network::Connection::State::Closing) {
                    ENVOY_LOG(info, "read_callbacks_ CLOSED");

                    if (!connection_close_) {
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
                        connection_close_ = true;
                    }
                }
                if (write_callbacks_->connection().state() == Network::Connection::State::Closed ||
                        write_callbacks_->connection().state() == Network::Connection::State::Closing) {
                    ENVOY_LOG(info, "write_callbacks_ CLOSED");

                    if (!connection_close_) {
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
                        connection_close_ = true;
                    }
                }
        }
    }

    void onAboveWriteBufferHighWatermark() override {
    }

    void onBelowWriteBufferLowWatermark() override {
    }
  
    // Destructor
    ~SenderFilter() {
        ENVOY_LOG(info, "DESTRUCTOR");
        close(sock_rdma_);
        close(sock_distant_rdma_);
    }

    // Constructor
    SenderFilter(const std::string& upstream_ip, uint32_t upstream_port, Envoy::Thread::ThreadFactory& thread_factory)
        : upstream_ip_(upstream_ip), upstream_port_(upstream_port), thread_factory_(thread_factory) {
        
        ENVOY_LOG(info, "CONSTRUCTOR");
        ENVOY_LOG(debug, "upstream_ip: {}", upstream_ip_);
        ENVOY_LOG(debug, "upstream_port: {}", upstream_port_);
    }

    void rdma_polling() {
        // Accept upstream RDMA connection
        listen(sock_rdma_, 1);
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        ENVOY_LOG(info, "Waiting for incoming RDMA connection...");
        sock_distant_rdma_ = accept(sock_rdma_, reinterpret_cast<struct sockaddr*>(&client_addr), &client_addr_len);
        ENVOY_LOG(info, "RDMA Connection from upstream listener accepted");

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
            int ret = poll(poll_fds, 1, 0);

            if (ret < 0) {
                ENVOY_LOG(error, "Poll error");
            }

            else if (ret == 0) {
            }

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
                std::string message(buffer, bytes_received);
                ENVOY_LOG(debug, "Received message from RDMA upstream: {}", message);
                bool pushed = false;
                while (!pushed) {
                    pushed = upstream_to_downstream_buffer_.push(message);
                }
            }
        }
        // rdma_polling_thread_ = nullptr;
        ENVOY_LOG(info, "rdma_polling stopped");
    }

    void rdma_sender() {
        while (active_rdma_sender_) {
            std::string item;
            if (downstream_to_upstream_buffer_.pop(item)) {
                ENVOY_LOG(debug, "Got item: {}", item);
                send(sock_distant_rdma_, item.c_str(), size(item), 0); // sock_distant_rdma_ must not be closed
            }
        }
        // rdma_sender_thread_ = nullptr;
        ENVOY_LOG(info, "rdma_sender stopped");
    }

    void downstream_sender() {
        while (active_downstream_sender_) {
            std::string item;
            if (upstream_to_downstream_buffer_.pop(item)) {
                ENVOY_LOG(debug, "Got item: {}", item);
                
                auto weak_self = weak_from_this();
                dispatcher_->post([weak_self, buffer = std::make_shared<Buffer::OwnedImpl>(item)]() -> void {
                    if (auto self = weak_self.lock()) {
                        self->write_callbacks_->injectWriteDataToFilterChain(*buffer, false);
                    }
                });
            }
        }
        // downstream_sender_thread_ = nullptr;
        ENVOY_LOG(info, "downstream_sender stopped");
    }

    // Thread-safe non-blocking Circular buffer
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

private:
    Network::ReadFilterCallbacks* read_callbacks_{};
    Network::WriteFilterCallbacks* write_callbacks_{};

    std::string upstream_ip_;
    uint32_t upstream_port_;
    bool connection_init_{true};
    bool connection_close_{false};
    int sock_rdma_;
    int sock_distant_rdma_;

    CircularBuffer<std::string> downstream_to_upstream_buffer_{1024};
    CircularBuffer<std::string> upstream_to_downstream_buffer_{1024};

    std::atomic<bool> active_rdma_polling_{true};
    std::atomic<bool> active_rdma_sender_{true};
    std::atomic<bool> active_downstream_sender_{true};

    Envoy::Thread::ThreadPtr rdma_polling_thread_;
    Envoy::Thread::ThreadPtr rdma_sender_thread_;
    Envoy::Thread::ThreadPtr downstream_sender_thread_;

    Envoy::Event::Dispatcher* dispatcher_{};
    Envoy::Thread::ThreadFactory& thread_factory_;
};

} // namespace Sender
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
