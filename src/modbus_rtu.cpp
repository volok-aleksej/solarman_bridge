#include "modbus_rtu.hpp"

#include <spdlog/fmt/fmt.h>

namespace modbus {

namespace {

const char* exception_name(uint8_t code) {
    switch (code) {
        case 0x01: return "illegal function";
        case 0x02: return "illegal data address";
        case 0x03: return "illegal data value";
        case 0x04: return "slave device failure";
        case 0x05: return "acknowledge";
        case 0x06: return "slave device busy";
        case 0x08: return "memory parity error";
        case 0x0A: return "gateway path unavailable";
        case 0x0B: return "gateway target failed to respond";
        default:   return "unknown exception";
    }
}

bool crc_ok(const uint8_t* data, size_t len) {
    if (len < 3) return false;
    const uint16_t crc = crc16(data, len - 2);
    return data[len - 2] == (crc & 0xFF) && data[len - 1] == (crc >> 8);
}

}  // namespace

uint16_t crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

namespace {

// Checks for an exception reply and for the expected length (allowing the
// double-CRC quirk: a valid frame followed by two extra zero bytes) and
// the CRC.
bool check_frame(const std::vector<uint8_t>& frame, uint8_t function,
                 size_t expected, std::string& error) {
    if (frame.size() >= 5 && frame[1] == (function | 0x80)) {
        error = fmt::format("modbus exception 0x{:02x} ({})", frame[2],
                            exception_name(frame[2]));
        return false;
    }

    size_t len = frame.size();
    if (len == expected + 2 && frame[len - 1] == 0 && frame[len - 2] == 0 &&
        crc_ok(frame.data(), expected))
        len = expected;

    if (len != expected) {
        error = fmt::format("modbus frame length {} (expected {})",
                            frame.size(), expected);
        return false;
    }
    if (!crc_ok(frame.data(), len)) {
        error = "modbus CRC mismatch";
        return false;
    }
    return true;
}

void append_crc(std::vector<uint8_t>& f) {
    const uint16_t crc = crc16(f.data(), f.size());
    f.push_back(crc & 0xFF);
    f.push_back(crc >> 8);
}

}  // namespace

std::vector<uint8_t> build_read_holding(uint8_t slave, uint16_t addr,
                                        uint16_t quantity) {
    std::vector<uint8_t> f = {
        slave, kReadHoldingRegisters,
        static_cast<uint8_t>(addr >> 8),     static_cast<uint8_t>(addr & 0xFF),
        static_cast<uint8_t>(quantity >> 8), static_cast<uint8_t>(quantity & 0xFF),
    };
    append_crc(f);
    return f;
}

bool parse_read_holding(const std::vector<uint8_t>& frame, uint8_t slave,
                        uint16_t quantity, std::vector<uint16_t>& out,
                        std::string& error) {
    if (!check_frame(frame, kReadHoldingRegisters, 5 + 2u * quantity, error))
        return false;
    if (frame[0] != slave || frame[1] != kReadHoldingRegisters ||
        frame[2] != 2 * quantity) {
        error = fmt::format("modbus header mismatch: slave={} func=0x{:02x} bytes={}",
                            frame[0], frame[1], frame[2]);
        return false;
    }

    out.resize(quantity);
    for (uint16_t i = 0; i < quantity; ++i)
        out[i] = (frame[3 + 2 * i] << 8) | frame[4 + 2 * i];
    return true;
}

std::vector<uint8_t> build_write_multiple(uint8_t slave, uint16_t addr,
                                          const std::vector<uint16_t>& values) {
    const uint16_t qty = values.size();
    std::vector<uint8_t> f = {
        slave, kWriteMultipleRegisters,
        static_cast<uint8_t>(addr >> 8), static_cast<uint8_t>(addr & 0xFF),
        static_cast<uint8_t>(qty >> 8),  static_cast<uint8_t>(qty & 0xFF),
        static_cast<uint8_t>(2 * qty),
    };
    for (uint16_t v : values) {
        f.push_back(v >> 8);
        f.push_back(v & 0xFF);
    }
    append_crc(f);
    return f;
}

bool parse_write_multiple(const std::vector<uint8_t>& frame, uint8_t slave,
                          uint16_t addr, uint16_t quantity, std::string& error) {
    if (!check_frame(frame, kWriteMultipleRegisters, 8, error)) return false;
    const uint16_t a = (frame[2] << 8) | frame[3];
    const uint16_t q = (frame[4] << 8) | frame[5];
    if (frame[0] != slave || frame[1] != kWriteMultipleRegisters ||
        a != addr || q != quantity) {
        error = fmt::format("modbus write echo mismatch: slave={} func=0x{:02x} "
                            "addr={} qty={}", frame[0], frame[1], a, q);
        return false;
    }
    return true;
}

}  // namespace modbus
