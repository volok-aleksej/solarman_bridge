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

const char* kLogger = R"(logger "roof" { host = "10.0.0.5"  serial = 123 }
)";

}  // namespace

TEST(Config, TwoInvertersOnOneLogger) {
    AppConfig cfg;
    std::string err;
    ASSERT_TRUE(load(std::string(kLogger) + R"(
inverter "a" { logger = "roof"  model = "deye_sg04lp3" }
inverter "b" { logger = "roof"  model = "deye_sg04lp3"  slave_id = 2  poll_period_ms = 2000 }
)", cfg, err)) << err;
    ASSERT_EQ(cfg.loggers.size(), 1u);
    EXPECT_EQ(cfg.loggers[0].name, "roof");
    EXPECT_EQ(cfg.loggers[0].port, 8899);
    ASSERT_EQ(cfg.inverters.size(), 2u);
    EXPECT_EQ(cfg.inverters[0].slave_id, 1);
    EXPECT_EQ(cfg.inverters[1].name, "b");
    EXPECT_EQ(cfg.inverters[1].slave_id, 2);
    EXPECT_EQ(cfg.inverters[1].poll_period_ms, 2000);
}

TEST(Config, RejectsBadReferences) {
    struct Case { const char* text; const char* error; };
    const Case cases[] = {
        {"", "at least one inverter"},
        {R"(inverter "a" { logger = "nope"  model = "deye_sg04lp3" })", "logger \"nope\" is not defined"},
        {R"(inverter "a" { logger = "roof"  model = "growatt" })", "model must be one of"},
        {R"(inverter "a" { logger = "roof"  model = "deye_sg04lp3" }
            inverter "b" { logger = "roof"  model = "deye_sg04lp3" })", "slave_id 1 is already used"},
    };
    for (const Case& c : cases) {
        AppConfig cfg;
        std::string err;
        EXPECT_FALSE(load(std::string(kLogger) + c.text, cfg, err)) << c.text;
        EXPECT_NE(err.find(c.error), std::string::npos) << err;
    }
}

TEST(Config, RequiresLoggerHostAndSerial) {
    AppConfig cfg;
    std::string err;
    EXPECT_FALSE(load(R"(logger "roof" { serial = 1 }
inverter "a" { logger = "roof"  model = "deye_sg04lp3" })", cfg, err));
    EXPECT_NE(err.find("host is required"), std::string::npos) << err;
}
