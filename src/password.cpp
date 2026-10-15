#include "password.hpp"

#include <crypt.h>

#include <cstring>

bool password_matches(const std::string& password, const std::string& hash) {
    if (hash.empty() || hash[0] != '$') return false;
    crypt_data data{};
    const char* out = crypt_r(password.c_str(), hash.c_str(), &data);
    // crypt_r reports a failure as NULL or as a string starting with '*'.
    if (!out || out[0] == '*') return false;
    const size_t n = std::strlen(out);
    if (n != hash.size()) return false;
    // Constant time over the whole string.
    unsigned char diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= out[i] ^ hash[i];
    return diff == 0;
}

void LoginGuard::failed(Clock::time_point now) {
    if (++failures_ >= kMaxFailures) {
        failures_     = 0;
        locked_until_ = now + kLockout;
    }
}
