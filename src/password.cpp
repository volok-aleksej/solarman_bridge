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

std::optional<std::string> basic_auth_password(const char* header) {
    static const char kScheme[] = "Basic ";
    if (!header || std::strncmp(header, kScheme, sizeof(kScheme) - 1) != 0) return std::nullopt;

    // Base64, padding optional.
    std::string decoded;
    unsigned acc = 0;
    int bits = 0;
    for (const char* p = header + sizeof(kScheme) - 1; *p && *p != '='; ++p) {
        int v;
        if      (*p >= 'A' && *p <= 'Z') v = *p - 'A';
        else if (*p >= 'a' && *p <= 'z') v = *p - 'a' + 26;
        else if (*p >= '0' && *p <= '9') v = *p - '0' + 52;
        else if (*p == '+')              v = 62;
        else if (*p == '/')              v = 63;
        else return std::nullopt;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    const size_t colon = decoded.find(':');
    if (colon == std::string::npos) return std::nullopt;
    return decoded.substr(colon + 1);
}

void LoginGuard::failed(Clock::time_point now) {
    if (++failures_ >= kMaxFailures) {
        failures_     = 0;
        locked_until_ = now + kLockout;
    }
}
