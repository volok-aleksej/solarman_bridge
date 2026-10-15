#include "password.hpp"

#include <gtest/gtest.h>

namespace {

// openssl passwd -6 -salt testsalt secret
const char* kHash = "$6$testsalt$51yaaBMXXXt5vK522YOlIMZ267vqHtMIzc2klcsu3EEv/"
                    "bkRDU9g3UmkypXf.NvlpPpIPK1nX5zdbCeJBiQbB/";

}  // namespace

TEST(Password, MatchesOpensslHash) {
    EXPECT_TRUE(password_matches("secret", kHash));
    EXPECT_FALSE(password_matches("Secret", kHash));
    EXPECT_FALSE(password_matches("", kHash));
}

TEST(Password, RejectsMalformedHashes) {
    EXPECT_FALSE(password_matches("secret", ""));
    EXPECT_FALSE(password_matches("secret", "secret"));     // a plain password
    EXPECT_FALSE(password_matches("secret", "$6$"));
}

TEST(LoginGuard, LocksAfterTooManyFailures) {
    LoginGuard g;
    auto t = LoginGuard::Clock::now();
    for (int i = 0; i < LoginGuard::kMaxFailures - 1; ++i) g.failed(t);
    EXPECT_FALSE(g.locked(t));
    g.failed(t);
    EXPECT_TRUE(g.locked(t));
    EXPECT_FALSE(g.locked(t + LoginGuard::kLockout));
}

TEST(LoginGuard, SuccessResetsTheCount) {
    LoginGuard g;
    auto t = LoginGuard::Clock::now();
    for (int i = 0; i < LoginGuard::kMaxFailures - 1; ++i) g.failed(t);
    g.succeeded();
    g.failed(t);
    EXPECT_FALSE(g.locked(t));
}
