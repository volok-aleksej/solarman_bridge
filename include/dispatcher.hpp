#pragma once

#include "event.hpp"
#include "timer.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

class Dispatcher {
public:
    using Handler   = std::function<void()>;
    using IoHandler = std::function<void(uint32_t events)>;

    Dispatcher();
    ~Dispatcher();
    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;

    void watch(int fd, Handler on_readable);
    // `events` is an EPOLL* mask; the handler gets the events that fired.
    void watch(int fd, uint32_t events, IoHandler on_event);
    void modify(int fd, uint32_t events);
    // Safe from inside a handler: events of this fd still pending in the
    // current batch are dropped, even if the fd number is reused at once.
    void unwatch(int fd);

    void add_timer(int period_ms, Handler on_tick);

    void run();
    void stop();

private:
    int epfd_     = -1;
    int signalfd_ = -1;
    Event wake_event_;
    std::atomic<bool> stopping_{false};
    std::map<int, IoHandler> handlers_;
    std::vector<int> unwatched_;  // fds removed during the current batch
    std::vector<std::unique_ptr<Timer>> timers_;
};
