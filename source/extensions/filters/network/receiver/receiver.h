#pragma once

#include "envoy/network/filter.h"
#include "envoy/buffer/buffer.h"
#include "envoy/network/connection.h"

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
 * Implementation of a custom receiver filter
 */
class ReceiverFilter : public Network::ReadFilter, public Network::WriteFilter,
                       public Network::ConnectionCallbacks, Logger::Loggable<Logger::Id::filter> {
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
        write_callbacks_->connection().addConnectionCallbacks(*this);
    }

    // Events
    void onEvent(Network::ConnectionEvent event) override {
        if (event == Network::ConnectionEvent::RemoteClose ||
            event == Network::ConnectionEvent::LocalClose) {
                if (read_callbacks_->connection().state() == Network::Connection::State::Closed ||
                    read_callbacks_->connection().state() == Network::Connection::State::Closing) {
                    ENVOY_LOG(debug, "read_callbacks_ CLOSED");
                }
                else if (write_callbacks_->connection().state() == Network::Connection::State::Closed ||
                        write_callbacks_->connection().state() == Network::Connection::State::Closing) {
                    ENVOY_LOG(debug, "write_callbacks_ CLOSED");
                }
        }
    }

    void onAboveWriteBufferHighWatermark() override {
    }

    void onBelowWriteBufferLowWatermark() override {
    }
  
    // Destructor
    ~ReceiverFilter() {
        ENVOY_LOG(debug, "DESTRUCTOR");
    }

    // Constructor
    ReceiverFilter(const std::string& upstream_ip, uint32_t upstream_port)
        : upstream_ip_(upstream_ip), upstream_port_(upstream_port) {
            
        ENVOY_LOG(debug, "upstream_ip: {}", upstream_ip_);
        ENVOY_LOG(debug, "upstream_port: {}", upstream_port_);
    }

    void rdma_polling() {
        // Start polling
        const int BUFFER_SIZE = 1024;
        char buffer[BUFFER_SIZE];
        int bytes_received;

        struct pollfd poll_fds[1];
        poll_fds[0].fd = sock_rdma_;
        poll_fds[0].events = POLLIN;

        while (true) {
            int ret = poll(poll_fds, 1, 0);

            if (ret < 0) {
                ENVOY_LOG(error, "Poll error");
            }

            else if (ret == 0) {
                }

            else if (poll_fds[0].revents & POLLIN) {
                memset(buffer, '\0', BUFFER_SIZE);
                bytes_received = recv(sock_rdma_, buffer, BUFFER_SIZE, 0);
                if (bytes_received < 0) {
                    ENVOY_LOG(info, "Error receiving message from RDMA upstream");
                    continue;
                } 
                else if (bytes_received == 0) {
                    ENVOY_LOG(info, "RDMA Upstream closed the connection");
                    continue;
                }
                std::string message(buffer, bytes_received);
                ENVOY_LOG(info, "Received message from RDMA upstream: {}", message);
                downstream_to_upstream_buffer_.put(message);
            }
        }
    }

    void rdma_sender() {
        while (true) {
            std::string item = upstream_to_downstream_buffer_.get();
            ENVOY_LOG(debug, "Got item: {}", item);
            send(sock_rdma_, item.c_str(), size(item), 0);
        }
    }

    void upstream_sender() {
        while (true) {
            std::string item = downstream_to_upstream_buffer_.get();
            ENVOY_LOG(debug, "Got item: {}", item);
            Buffer::OwnedImpl buffer(item);
            read_callbacks_->injectReadDataToFilterChain(buffer, false); //Seg fault if close client
            read_callbacks_->continueReading();
        }
    }

    void splitString(const std::string& inputString, std::string& firstPart, std::string& secondPart)
    {
        size_t pipePosition = inputString.find('|');
        
        if (pipePosition != std::string::npos)
        {
            firstPart = inputString.substr(0, pipePosition);
            secondPart = inputString.substr(pipePosition + 1);
        }
        else
        {
            firstPart = inputString;
            secondPart.clear();
        }
    }

    // Thread-safe Circular buffer
    #include <array>
    #include <mutex>
    #include <condition_variable>

    template <typename T, std::size_t N>
    class CircularBuffer
    {
    public:
        CircularBuffer() : head(0), tail(0), count(0) {}

        void put(const T& item)
        {
            std::unique_lock<std::mutex> lock(mutex);
            notFull.wait(lock, [this](){ return count < N; });
            buffer[tail] = item;
            tail = (tail + 1) % N;
            ++count;
            notEmpty.notify_one();
        }

        T get()
        {
            std::unique_lock<std::mutex> lock(mutex);
            notEmpty.wait(lock, [this](){ return count > 0; });
            T item = buffer[head];
            head = (head + 1) % N;
            --count;
            notFull.notify_one();
            return item;
        }

    private:
        std::array<T, N> buffer;
        std::mutex mutex;
        std::condition_variable notFull;
        std::condition_variable notEmpty;
        std::size_t head;
        std::size_t tail;
        std::size_t count;
    };

private:
    Network::ReadFilterCallbacks* read_callbacks_{};
    Network::WriteFilterCallbacks* write_callbacks_{};
    std::string upstream_ip_;
    uint32_t upstream_port_;
    bool connection_init_{false};
    int sock_rdma_;
    CircularBuffer<std::string, 10> downstream_to_upstream_buffer_;
    CircularBuffer<std::string, 10> upstream_to_downstream_buffer_;
};

} // namespace Receiver
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
