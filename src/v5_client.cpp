#include "v5_client.hpp"

#include "modbus_rtu.hpp"
#include "solarman_v5.hpp"

#include <spdlog/spdlog.h>

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <random>

namespace {

using Clock = std::chrono::steady_clock;

std::string hex(const uint8_t* p, size_t n) {
    std::string s;
    s.reserve(n * 3);
    char b[4];
    for (size_t i = 0; i < n; ++i) {
        snprintf(b, sizeof(b), i ? " %02x" : "%02x", p[i]);
        s += b;
    }
    return s;
}

}  // namespace

V5Client::V5Client(std::string host, uint16_t port, uint32_t logger_serial,
                   uint8_t slave_id, std::chrono::milliseconds timeout,
                   int cancel_fd)
    : host_(std::move(host)), port_(port), logger_serial_(logger_serial),
      slave_id_(slave_id), timeout_(timeout), cancel_fd_(cancel_fd) {
    // Random initial sequence number, like pysolarmanv5: a fresh process
    // must not match a reply the logger still has queued for the old one.
    std::random_device rd;
    seq_ = std::uniform_int_distribution<int>(1, 0xFE)(rd);
}

V5Client::~V5Client() { close(); }

void V5Client::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        spdlog::debug("v5: connection to {}:{} closed", host_, port_);
    }
    rx_.clear();
}

V5Client::Wait V5Client::wait_fd(short events, Clock::time_point deadline) {
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - Clock::now()).count();
        if (left <= 0) return Wait::Timeout;

        pollfd pfd[2] = {{fd_, events, 0}, {cancel_fd_, POLLIN, 0}};
        const int n = ::poll(pfd, 2, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return Wait::Error;
        }
        if (n == 0) return Wait::Timeout;
        if (pfd[1].revents) return Wait::Cancelled;
        if (pfd[0].revents) return Wait::Ready;
    }
}

bool V5Client::connect(std::string& error) {
    close();

    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string port_s = std::to_string(port_);
    if (int rc = getaddrinfo(host_.c_str(), port_s.c_str(), &hints, &res); rc != 0) {
        error = std::string("resolve ") + host_ + ": " + gai_strerror(rc);
        return false;
    }

    fd_ = ::socket(res->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        freeaddrinfo(res);
        return false;
    }

    int rc = ::connect(fd_, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0 && errno != EINPROGRESS) {
        error = std::string("connect: ") + std::strerror(errno);
        close();
        return false;
    }

    if (rc < 0) {
        switch (wait_fd(POLLOUT, Clock::now() + timeout_)) {
            case Wait::Ready: break;
            case Wait::Timeout:   error = "connect: timeout";   close(); return false;
            case Wait::Cancelled: error = "connect: cancelled"; close(); return false;
            case Wait::Error:
                error = std::string("connect: poll: ") + std::strerror(errno);
                close();
                return false;
        }
        int so_err = 0;
        socklen_t sl = sizeof(so_err);
        getsockopt(fd_, SOL_SOCKET, SO_ERROR, &so_err, &sl);
        if (so_err != 0) {
            error = std::string("connect: ") + std::strerror(so_err);
            close();
            return false;
        }
    }

    int one = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

    spdlog::info("v5: connected to logger {}:{} (serial {})",
                 host_, port_, logger_serial_);
    return true;
}

bool V5Client::send_all(const std::vector<uint8_t>& data,
                        Clock::time_point deadline, std::string& error) {
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::send(fd_, data.data() + off, data.size() - off,
                                 MSG_NOSIGNAL);
        if (n > 0) { off += n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno != EAGAIN) {
            error = std::string("send: ") + std::strerror(errno);
            return false;
        }
        if (wait_fd(POLLOUT, deadline) != Wait::Ready) {
            error = "send: timeout";
            return false;
        }
    }
    return true;
}

bool V5Client::receive_frame(uint8_t seq, std::vector<uint8_t>& modbus_out,
                             Clock::time_point deadline, std::string& error) {
    for (;;) {
        // Consume every complete frame already buffered.
        for (;;) {
            size_t junk = 0;
            const size_t len = v5::frame_length(rx_, junk);
            if (junk) {
                spdlog::debug("v5: dropping {} byte(s) of junk", junk);
                rx_.erase(rx_.begin(), rx_.begin() + junk);
                continue;
            }
            if (len == 0) break;

            spdlog::trace("v5: RECV {}", hex(rx_.data(), len));
            const auto r = v5::decode_response(rx_.data(), len, seq,
                                               logger_serial_, modbus_out, error);
            rx_.erase(rx_.begin(), rx_.begin() + len);
            if (r == v5::DecodeResult::Ok)    return true;
            if (r == v5::DecodeResult::Error) return false;
            spdlog::debug("v5: ignoring unsolicited/stale frame");
        }

        switch (wait_fd(POLLIN, deadline)) {
            case Wait::Ready: break;
            case Wait::Timeout:   error = "receive: timeout";   return false;
            case Wait::Cancelled: error = "receive: cancelled"; return false;
            case Wait::Error:
                error = std::string("receive: poll: ") + std::strerror(errno);
                return false;
        }

        uint8_t buf[1024];
        const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        if (n == 0) { error = "receive: logger closed the connection"; return false; }
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            error = std::string("receive: ") + std::strerror(errno);
            return false;
        }
        rx_.insert(rx_.end(), buf, buf + n);
    }
}

bool V5Client::read_holding(uint16_t addr, uint16_t quantity,
                            std::vector<uint16_t>& out, std::string& error) {
    if (quantity == 0 || quantity > modbus::kMaxReadQuantity) {
        error = "read_holding: bad quantity";
        return false;
    }
    if (fd_ < 0 && !connect(error)) return false;

    seq_ = seq_ == 0xFF ? 1 : seq_ + 1;
    const auto mb_req = modbus::build_read_holding(slave_id_, addr, quantity);
    const auto frame  = v5::encode_request(seq_, logger_serial_, mb_req);
    spdlog::trace("v5: SEND {}", hex(frame.data(), frame.size()));

    const auto deadline = Clock::now() + timeout_;
    std::vector<uint8_t> mb_resp;
    if (!send_all(frame, deadline, error) ||
        !receive_frame(seq_, mb_resp, deadline, error)) {
        error = "regs " + std::to_string(addr) + "+" + std::to_string(quantity)
                + ": " + error;
        close();
        return false;
    }

    if (!modbus::parse_read_holding(mb_resp, slave_id_, quantity, out, error)) {
        // The TCP stream is still in sync; only this answer was bad.
        error = "regs " + std::to_string(addr) + "+" + std::to_string(quantity)
                + ": " + error;
        return false;
    }
    return true;
}
