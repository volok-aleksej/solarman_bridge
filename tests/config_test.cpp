#include "config.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>

namespace {

// Loads `text` as a config file.
bool load(const std::string& text, AppConfig& cfg, std::string& err) {
    const std::string path = "/tmp/solarman_config_test_" + std::to_string(getpid()) + ".conf";
    std::ofstream(path) << text;
    const bool ok = load_config(path, cfg, err);
    std::remove(path.c_str());
    return ok;
}

}  // namespace

TEST(Config, TwoInverters) {
    AppConfig cfg;
    std::string err;
    ASSERT_TRUE(load(R"(
inverter "a" { model = "deye_sg04lp3"  host = "10.0.0.5"  serial = 123 }
inverter "b" { model = "deye_sg04lp3"  host = "10.0.0.6"  serial = 456  poll_period_ms = 2000 }
)", cfg, err)) << err;
    ASSERT_EQ(cfg.inverters.size(), 2u);
    EXPECT_EQ(cfg.inverters[0].name, "a");
    EXPECT_EQ(cfg.inverters[0].port, 8899);
    EXPECT_EQ(cfg.inverters[0].slave_id, 1);
    EXPECT_EQ(cfg.inverters[1].host, "10.0.0.6");
    EXPECT_EQ(cfg.inverters[1].serial, 456u);
    EXPECT_EQ(cfg.inverters[1].poll_period_ms, 2000);
}

TEST(Config, RejectsBadInverters) {
    struct Case { const char* text; const char* error; };
    const Case cases[] = {
        {"", "at least one inverter"},
        {R"(inverter "a" { model = "growatt"  host = "h"  serial = 1 })", "model must be one of"},
        {R"(inverter "a" { model = "deye_sg04lp3"  serial = 1 })", "host is required"},
        {R"(inverter "a" { model = "deye_sg04lp3"  host = "h" })", "serial is required"},
    };
    for (const Case& c : cases) {
        AppConfig cfg;
        std::string err;
        EXPECT_FALSE(load(c.text, cfg, err)) << c.text;
        EXPECT_NE(err.find(c.error), std::string::npos) << err;
    }
}

TEST(Config, ControlNeedsAPasswordHash) {
    const std::string inv = R"(inverter "a" { model = "deye_sg04lp3"  host = "h"  serial = 1 }
)";
    AppConfig cfg;
    std::string err;
    EXPECT_FALSE(load(inv + "control { enabled = true }", cfg, err));
    EXPECT_NE(err.find("password_hash"), std::string::npos) << err;
    // A real openssl hash survives the parser unchanged ($ is not special).
    const std::string hash = "$6$testsalt$51yaaBMXXXt5vK522YOlIMZ267vqHtMIzc2klcsu3EEv/"
                             "bkRDU9g3UmkypXf.NvlpPpIPK1nX5zdbCeJBiQbB/";
    EXPECT_TRUE(load(inv + "control { enabled = true  password_hash = \"" + hash + "\" }",
                     cfg, err)) << err;
    EXPECT_TRUE(cfg.control.enabled);
    EXPECT_EQ(cfg.control.password_hash, hash);
    EXPECT_TRUE(load(inv + "control { enabled = false }", cfg, err)) << err;
}
