#pragma once

#include <chrono>
#include <string>

// True if `password` matches `hash`, a crypt(3) string such as the one
// `openssl passwd -6` prints ("$6$salt$..."). False for any malformed hash.
bool password_matches(const std::string& password, const std::string& hash);

// Refuses logins for a while after too many wrong passwords in a row, so
// the password cannot be guessed at network speed. Single-threaded.
class LoginGuard {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr int  kMaxFailures = 5;
    static constexpr auto kLockout     = std::chrono::seconds(60);

    bool locked(Clock::time_point now) const { return now < locked_until_; }
    void failed(Clock::time_point now);
    void succeeded() { failures_ = 0; }

private:
    int               failures_ = 0;
    Clock::time_point locked_until_{};
};
