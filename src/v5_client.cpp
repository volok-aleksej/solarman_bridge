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
                   uint32_t logger_serial, std::chrono::milliseconds timeout,
                   std::chrono::milliseconds reconnect, size_t max_queue)
    : dispatcher_(dispatcher), host_(std::move(host)), port_(port),
      logger_serial_(logger_serial), timeout_(timeout), reconnect_(reconnect),
      max_queue_(max_queue) {
    // Random initial sequence number, like pysolarmanv5: a fresh process
    // must not match a reply the logger still has queued for the old one.
    std::random_device rd;
    seq_ = std::uniform_int_distribution<int>(1, 0xFE)(rd);
}

V5Client::~V5Client() { close_socket(); }

bool V5Client::submit(std::vector<uint8_t> modbus_frame, Reply done, bool urgent) {
    if (!connected()) return false;
    if (queue_.size() >= max_queue_) {
        if (!queue_full_logged_)
            spdlog::warn("v5: request queue full ({}), dropping requests; "
                         "the poll period is too short for the link", max_queue_);
        queue_full_logged_ = true;
        return false;
    }
    queue_full_logged_ = false;
    Request r{std::move(modbus_frame), std::move(done)};
    if (urgent) queue_.push_front(std::move(r));
    else        queue_.push_back(std::move(r));
    return true;
}

void V5Client::tick() {
    const auto now = Clock::now();
    switch (state_) {
        case State::Closed:
            if (now >= retry_at_) connect();
            break;
        case State::Connecting:
            if (now >= deadline_) fail("connect: timeout");
            break;
        case State::Awaiting:
            if (now >= deadline_) on_timeout();
            break;
        case State::Idle:
            send_next();
            break;
    }
}

// A silent inverter on a shared logger must not cut off the others, so a
// timeout fails only its request; several in a row mean the link is dead.
void V5Client::on_timeout() {
    if (++timeouts_in_row_ >= kTimeoutsToDrop) {
        fail("receive: timeout");
        return;
    }
    spdlog::debug("v5: receive: timeout, link kept");
    Request r = std::move(current_);
    current_ = {};
    state_ = State::Idle;
    r.done(false, {}, "receive: timeout");
    if (state_ == State::Idle) send_next();
}

void V5Client::close_socket() {
    if (fd_ >= 0) {
        dispatcher_.unwatch(fd_);
        ::close(fd_);
        fd_ = -1;
        spdlog::debug("v5: connection to {}:{} closed", host_, port_);
    }
    state_ = State::Closed;
    watched_events_ = 0;
    timeouts_in_row_ = 0;
    tx_.clear();
    rx_.clear();
}

void V5Client::fail(const std::string& error) {
    const bool was_up = connected();
    last_error_ = error;
    close_socket();
    retry_at_ = Clock::now() + reconnect_;

    if (failing_) spdlog::debug("v5: {}", error);
    else          spdlog::warn("v5: {}{}", error, was_up ? "" : " (connecting)");
    failing_ = true;

    if (current_.done) current_.done(false, {}, error);
    current_ = {};
    for (auto& r : queue_) r.done(false, {}, error);
    queue_.clear();
}

void V5Client::connect() {
    // getaddrinfo blocks only when the host is a name, not an address.
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string port_s = std::to_string(port_);
    if (int rc = getaddrinfo(host_.c_str(), port_s.c_str(), &hints, &res); rc != 0) {
        fail("resolve " + host_ + ": " + gai_strerror(rc));
        return;
    }
    fd_ = ::socket(res->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        freeaddrinfo(res);
        fail(errno_text("socket"));
        return;
    }
    const int rc  = ::connect(fd_, res->ai_addr, res->ai_addrlen);
    const int err = errno;
    freeaddrinfo(res);
    if (rc < 0 && err != EINPROGRESS) {
        fail(errno_text("connect", err));
        return;
    }

    // Writability reports the outcome, also for a connect done at once.
    state_    = State::Connecting;
    deadline_ = Clock::now() + timeout_;
    watched_events_ = EPOLLOUT;
    dispatcher_.watch(fd_, watched_events_, [this](uint32_t ev) { on_socket(ev); });
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
    state_ = State::Idle;
    failing_ = false;
    spdlog::info("v5: connected to logger {}:{} (serial {})",
                 host_, port_, logger_serial_);
}

void V5Client::send_next() {
    if (queue_.empty()) return;
    current_ = std::move(queue_.front());
    queue_.pop_front();
    seq_ = seq_ == 0xFF ? 1 : seq_ + 1;
    tx_  = v5::encode_request(seq_, logger_serial_, current_.frame);
    spdlog::trace("v5: SEND {}", hex(tx_.data(), tx_.size()));
    state_    = State::Awaiting;
    deadline_ = Clock::now() + timeout_;
    if (!flush_tx()) fail(errno_text("send"));
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

void V5Client::on_socket(uint32_t events) {
    if (state_ == State::Connecting) {
        finish_connect();
        return;
    }
    if ((events & EPOLLOUT) && !flush_tx()) {
        fail(errno_text("send"));
        return;
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
            fail(error);
            return;
        }
        // NoReply fails only this request: the link is fine, and other
        // inverters on the same logger may well answer.
        if ((r == v5::DecodeResult::Ok || r == v5::DecodeResult::NoReply) &&
            state_ == State::Awaiting) {
            Request done = std::move(current_);
            current_ = {};
            state_ = State::Idle;
            const bool ok = r == v5::DecodeResult::Ok;
            timeouts_in_row_ = 0;
            done.done(ok, std::move(modbus), ok ? std::string() : error);
            // The next request goes out at once, not on the next tick.
            if (state_ == State::Idle) send_next();
            return;
        }
        spdlog::debug("v5: ignoring unsolicited/stale frame");
    }
}
