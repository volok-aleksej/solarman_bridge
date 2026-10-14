#include "v5_client.hpp"

#include "dispatcher.hpp"
#include "solarman_v5.hpp"

#include <spdlog/spdlog.h>

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <random>
#include <stdexcept>

namespace {

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

std::string errno_text(const char* what, int err = errno) {
    return std::string(what) + ": " + std::strerror(err);
}

}  // namespace

V5Client::V5Client(Dispatcher& dispatcher, std::string host, uint16_t port,
                   uint32_t logger_serial, std::chrono::milliseconds timeout)
    : dispatcher_(dispatcher), host_(std::move(host)), port_(port),
      logger_serial_(logger_serial), timeout_(timeout) {
    // Random initial sequence number, like pysolarmanv5: a fresh process
    // must not match a reply the logger still has queued for the old one.
    std::random_device rd;
    seq_ = std::uniform_int_distribution<int>(1, 0xFE)(rd);
    dispatcher_.watch(timer_.fd(), [this] { on_timer(); });
}

V5Client::~V5Client() {
    close();
    dispatcher_.unwatch(timer_.fd());
}

void V5Client::close() {
    if (fd_ >= 0) {
        dispatcher_.unwatch(fd_);
        ::close(fd_);
        fd_ = -1;
        spdlog::debug("v5: connection to {}:{} closed", host_, port_);
    }
    timer_.disarm();
    state_ = State::Closed;
    watched_events_ = 0;
    deferred_error_.clear();
    tx_.clear();
    rx_.clear();
    connect_done_ = nullptr;
    reply_        = nullptr;
}

void V5Client::defer_error(std::string error) {
    deferred_error_ = std::move(error);
    timer_.arm(std::chrono::milliseconds(0));
}

void V5Client::fail(std::string error) {
    const State was = state_;
    Done  done  = std::move(connect_done_);
    Reply reply = std::move(reply_);
    close();
    if (was == State::Connecting && done)       done(false, error);
    else if (was == State::Awaiting && reply)   reply(false, {}, error);
    else spdlog::info("v5: {}", error);  // an idle connection went away
}

void V5Client::connect(Done done) {
    if (state_ != State::Closed)
        throw std::logic_error("V5Client::connect: not closed");
    state_        = State::Connecting;
    connect_done_ = std::move(done);

    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string port_s = std::to_string(port_);
    if (int rc = getaddrinfo(host_.c_str(), port_s.c_str(), &hints, &res); rc != 0) {
        defer_error("resolve " + host_ + ": " + gai_strerror(rc));
        return;
    }

    fd_ = ::socket(res->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        freeaddrinfo(res);
        defer_error(errno_text("socket"));
        return;
    }
    const int rc = ::connect(fd_, res->ai_addr, res->ai_addrlen);
    const int err = errno;
    freeaddrinfo(res);
    if (rc < 0 && err != EINPROGRESS) {
        defer_error(errno_text("connect", err));
        return;
    }

    // Writability reports the outcome, also for a connect that completed
    // at once.
    watched_events_ = EPOLLOUT;
    dispatcher_.watch(fd_, watched_events_, [this](uint32_t ev) { on_socket(ev); });
    timer_.arm(timeout_);
}

void V5Client::finish_connect() {
    int so_err = 0;
    socklen_t sl = sizeof(so_err);
    getsockopt(fd_, SOL_SOCKET, SO_ERROR, &so_err, &sl);
    if (so_err != 0) {
        fail(errno_text("connect", so_err));
        return;
    }

    int one = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

    // Keep reading while idle: counter frames and a close by the logger
    // are noticed right away.
    watched_events_ = EPOLLIN;
    dispatcher_.modify(fd_, watched_events_);
    timer_.disarm();
    state_ = State::Idle;
    spdlog::info("v5: connected to logger {}:{} (serial {})",
                 host_, port_, logger_serial_);
    Done done = std::move(connect_done_);
    if (done) done(true, {});
}

void V5Client::request(const std::vector<uint8_t>& modbus_frame, Reply done) {
    if (state_ != State::Idle)
        throw std::logic_error("V5Client::request: not idle");

    seq_ = seq_ == 0xFF ? 1 : seq_ + 1;
    tx_  = v5::encode_request(seq_, logger_serial_, modbus_frame);
    spdlog::trace("v5: SEND {}", hex(tx_.data(), tx_.size()));
    state_ = State::Awaiting;
    reply_ = std::move(done);
    timer_.arm(timeout_);
    if (!flush_tx()) defer_error(errno_text("send"));
}

// Sends what the socket takes; the rest waits for EPOLLOUT.
bool V5Client::flush_tx() {
    while (!tx_.empty()) {
        const ssize_t n = ::send(fd_, tx_.data(), tx_.size(), MSG_NOSIGNAL);
        if (n > 0) { tx_.erase(tx_.begin(), tx_.begin() + n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) break;
        return false;
    }
    const uint32_t want = tx_.empty() ? EPOLLIN : EPOLLIN | EPOLLOUT;
    if (want != watched_events_) {
        watched_events_ = want;
        dispatcher_.modify(fd_, want);
    }
    return true;
}

void V5Client::on_timer() {
    timer_.consume();
    if (!deferred_error_.empty()) {
        fail(std::move(deferred_error_));
        return;
    }
    if (state_ == State::Connecting) fail("connect: timeout");
    else if (state_ == State::Awaiting) fail("receive: timeout");
}

void V5Client::on_socket(uint32_t events) {
    if (state_ == State::Connecting) {
        finish_connect();
        return;
    }
    if (events & EPOLLOUT) {
        if (!flush_tx()) {
            fail(errno_text("send"));
            return;
        }
    }
    if (events & (EPOLLIN | EPOLLERR | EPOLLHUP)) read_socket();
}

void V5Client::read_socket() {
    for (;;) {
        uint8_t buf[1024];
        const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        if (n > 0) { rx_.insert(rx_.end(), buf, buf + n); continue; }
        if (n == 0) { fail("receive: logger closed the connection"); return; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN) break;
        fail(errno_text("receive"));
        return;
    }
    process_rx();
}

void V5Client::process_rx() {
    for (;;) {
        size_t junk = 0;
        const size_t len = v5::frame_length(rx_, junk);
        if (junk) {
            spdlog::debug("v5: dropping {} byte(s) of junk", junk);
            rx_.erase(rx_.begin(), rx_.begin() + junk);
            continue;
        }
        if (len == 0) return;

        spdlog::trace("v5: RECV {}", hex(rx_.data(), len));
        std::vector<uint8_t> modbus;
        std::string error;
        const auto r = v5::decode_response(rx_.data(), len, seq_,
                                           logger_serial_, modbus, error);
        rx_.erase(rx_.begin(), rx_.begin() + len);

        if (r == v5::DecodeResult::Error) {
            fail(std::move(error));
            return;
        }
        if (r == v5::DecodeResult::Ok && state_ == State::Awaiting) {
            timer_.disarm();
            state_ = State::Idle;
            Reply reply = std::move(reply_);
            reply(true, std::move(modbus), {});
            return;  // the callback may have changed everything
        }
        spdlog::debug("v5: ignoring unsolicited/stale frame");
    }
}
