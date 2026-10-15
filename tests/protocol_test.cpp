#include "modbus_rtu.hpp"
#include "solarman_v5.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

constexpr uint32_t kLoggerSerial = 3581987480;
constexpr uint8_t  kSeq = 0x44;

// Captured on the wire with pysolarmanv5 against the real logger:
// read_holding_registers(62, 3) → inverter clock 2026-10-14 16:39:21.
const std::vector<uint8_t> kRequest = {
    0xa5, 0x17, 0x00, 0x10, 0x45, 0x44, 0x00, 0x98, 0xca, 0x80, 0xd5, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x03, 0x00, 0x3e, 0x00, 0x03, 0x64, 0x07, 0x19, 0x15,
};
const std::vector<uint8_t> kResponse = {
    0xa5, 0x19, 0x00, 0x10, 0x15, 0x44, 0x31, 0x98, 0xca, 0x80, 0xd5, 0x02,
    0x01, 0x55, 0xaa, 0x8b, 0x30, 0xac, 0x1e, 0x00, 0x00, 0xb6, 0xdb, 0x43,
    0x3a, 0x01, 0x03, 0x06, 0x1a, 0x0a, 0x0e, 0x10, 0x27, 0x15, 0x62, 0x5c,
    0x45, 0x15,
};

std::vector<uint8_t> mb_response_of(const std::vector<uint8_t>& v5_frame) {
    return {v5_frame.begin() + 25, v5_frame.end() - 2};
}

}  // namespace

TEST(ModbusRtu, BuildReadHoldingMatchesCapture) {
    const auto f = modbus::build_read_holding(1, 62, 3);
    const std::vector<uint8_t> expected(kRequest.begin() + 26, kRequest.end() - 2);
    EXPECT_EQ(f, expected);
}

TEST(ModbusRtu, ParseReadHolding) {
    std::vector<uint16_t> regs;
    std::string err;
    ASSERT_TRUE(modbus::parse_read_holding(mb_response_of(kResponse), 1, 3, regs, err)) << err;
    EXPECT_EQ(regs, (std::vector<uint16_t>{0x1a0a, 0x0e10, 0x2715}));
}

TEST(ModbusRtu, RejectsBadCrc) {
    auto f = mb_response_of(kResponse);
    f.back() ^= 0xFF;
    std::vector<uint16_t> regs;
    std::string err;
    EXPECT_FALSE(modbus::parse_read_holding(f, 1, 3, regs, err));
    EXPECT_NE(err.find("CRC"), std::string::npos);
}

TEST(ModbusRtu, AcceptsDoubleCrcQuirk) {
    auto f = mb_response_of(kResponse);
    f.push_back(0);
    f.push_back(0);
    std::vector<uint16_t> regs;
    std::string err;
    ASSERT_TRUE(modbus::parse_read_holding(f, 1, 3, regs, err)) << err;
    EXPECT_EQ(regs.size(), 3u);
}

TEST(ModbusRtu, ReportsException) {
    std::vector<uint8_t> f = {0x01, 0x83, 0x02};
    const uint16_t crc = modbus::crc16(f.data(), f.size());
    f.push_back(crc & 0xFF);
    f.push_back(crc >> 8);
    std::vector<uint16_t> regs;
    std::string err;
    EXPECT_FALSE(modbus::parse_read_holding(f, 1, 3, regs, err));
    EXPECT_NE(err.find("illegal data address"), std::string::npos);
}

TEST(SolarmanV5, EncodeMatchesCapture) {
    const auto mb = modbus::build_read_holding(1, 62, 3);
    EXPECT_EQ(v5::encode_request(kSeq, kLoggerSerial, mb), kRequest);
}

TEST(SolarmanV5, DecodeCapturedResponse) {
    std::vector<uint8_t> mb;
    std::string err;
    ASSERT_EQ(v5::decode_response(kResponse.data(), kResponse.size(), kSeq,
                                  kLoggerSerial, mb, err),
              v5::DecodeResult::Ok) << err;
    EXPECT_EQ(mb, mb_response_of(kResponse));
}

TEST(SolarmanV5, StaleSequenceIsIgnored) {
    std::vector<uint8_t> mb;
    std::string err;
    EXPECT_EQ(v5::decode_response(kResponse.data(), kResponse.size(), kSeq + 1,
                                  kLoggerSerial, mb, err),
              v5::DecodeResult::Ignore);
}

TEST(SolarmanV5, CounterFrameIsIgnored) {
    auto f = kResponse;
    f[3] = 0x10;
    f[4] = 0x47;
    f[f.size() - 2] = v5::checksum(f.data(), f.size());
    std::vector<uint8_t> mb;
    std::string err;
    EXPECT_EQ(v5::decode_response(f.data(), f.size(), kSeq, kLoggerSerial, mb, err),
              v5::DecodeResult::Ignore);
}

TEST(SolarmanV5, BadChecksumIsError) {
    auto f = kResponse;
    f[f.size() - 2] ^= 0x01;
    std::vector<uint8_t> mb;
    std::string err;
    EXPECT_EQ(v5::decode_response(f.data(), f.size(), kSeq, kLoggerSerial, mb, err),
              v5::DecodeResult::Error);
}

TEST(SolarmanV5, WrongLoggerSerialIsError) {
    std::vector<uint8_t> mb;
    std::string err;
    EXPECT_EQ(v5::decode_response(kResponse.data(), kResponse.size(), kSeq,
                                  kLoggerSerial + 1, mb, err),
              v5::DecodeResult::Error);
}

TEST(SolarmanV5, EmptyModbusPayloadIsError) {
    // What the logger sends when the inverter does not answer on RS485.
    std::vector<uint8_t> f(kResponse.begin(), kResponse.begin() + 25);
    f[1] = 14;
    f[2] = 0;
    f.push_back(0);
    f.push_back(v5::kEnd);
    f[f.size() - 2] = v5::checksum(f.data(), f.size());
    std::vector<uint8_t> mb;
    std::string err;
    EXPECT_EQ(v5::decode_response(f.data(), f.size(), kSeq, kLoggerSerial, mb, err),
              v5::DecodeResult::Error);
}

TEST(SolarmanV5, FrameLengthSplitsStream) {
    size_t junk = 0;
    std::vector<uint8_t> buf = {0x00, 0x11};
    buf.insert(buf.end(), kResponse.begin(), kResponse.end());
    EXPECT_EQ(v5::frame_length(buf, junk), 0u);
    EXPECT_EQ(junk, 2u);

    buf.erase(buf.begin(), buf.begin() + 2);
    buf.insert(buf.end(), kResponse.begin(), kResponse.begin() + 5);  // next, partial
    EXPECT_EQ(v5::frame_length(buf, junk), kResponse.size());
    EXPECT_EQ(junk, 0u);

    std::vector<uint8_t> partial(kResponse.begin(), kResponse.begin() + 10);
    EXPECT_EQ(v5::frame_length(partial, junk), 0u);
    EXPECT_EQ(junk, 0u);
}

TEST(ModbusRtu, BuildWriteMultiple) {
    // Reference CRC computed independently.
    EXPECT_EQ(modbus::build_write_multiple(1, 80, {0}),
              (std::vector<uint8_t>{0x01, 0x10, 0x00, 0x50, 0x00, 0x01, 0x02,
                                    0x00, 0x00, 0xaa, 0x00}));
}

TEST(ModbusRtu, ParseWriteMultipleEcho) {
    const std::vector<uint8_t> echo = {0x01, 0x10, 0x00, 0x50, 0x00, 0x01, 0x01, 0xd8};
    std::string err;
    EXPECT_TRUE(modbus::parse_write_multiple(echo, 1, 80, 1, err)) << err;
    EXPECT_FALSE(modbus::parse_write_multiple(echo, 1, 81, 1, err));
    EXPECT_NE(err.find("echo mismatch"), std::string::npos);

    auto quirk = echo;
    quirk.push_back(0);
    quirk.push_back(0);
    EXPECT_TRUE(modbus::parse_write_multiple(quirk, 1, 80, 1, err)) << err;
}

TEST(ModbusRtu, ParseWriteMultipleException) {
    std::vector<uint8_t> f = {0x01, 0x90, 0x02};
    const uint16_t crc = modbus::crc16(f.data(), f.size());
    f.push_back(crc & 0xFF);
    f.push_back(crc >> 8);
    std::string err;
    EXPECT_FALSE(modbus::parse_write_multiple(f, 1, 80, 1, err));
    EXPECT_NE(err.find("illegal data address"), std::string::npos);
}
