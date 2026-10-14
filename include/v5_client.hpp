#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// Blocking Solarman V5 client over one persistent TCP connection.
//
// Every wait (connect, send, receive) is a poll() on the socket plus
// `cancel_fd`, so a signal on that eventfd aborts the call at once. Any
// failure closes the socket; the next call reconnects lazily.
class V5Client {
public:
    V5Client(std::string host, uint16_t port, uint32_t logger_serial,
             uint8_t slave_id, std::chrono::milliseconds timeout,
             int cancel_fd);
    ~V5Client();

    V5Client(const V5Client&) = delete;
    V5Client& operator=(const V5Client&) = delete;

    bool connected() const noexcept { return fd_ >= 0; }
    bool connect(std::string& error);
    void close();

    bool read_holding(uint16_t addr, uint16_t quantity,
                      std::vector<uint16_t>& out, std::string& error);

private:
    enum class Wait { Ready, Timeout, Cancelled, Error };
    Wait wait_fd(short events, std::chrono::steady_clock::time_point deadline);

    bool send_all(const std::vector<uint8_t>& data,
                  std::chrono::steady_clock::time_point deadline,
                  std::string& error);
    bool receive_frame(uint8_t seq, std::vector<uint8_t>& modbus_out,
                       std::chrono::steady_clock::time_point deadline,
                       std::string& error);

    std::string               host_;
    uint16_t                  port_;
    uint32_t                  logger_serial_;
    uint8_t                   slave_id_;
    std::chrono::milliseconds timeout_;
    int                       cancel_fd_;

    int                  fd_  = -1;
    uint8_t              seq_ = 0;
    std::vector<uint8_t> rx_;
};
