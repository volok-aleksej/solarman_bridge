#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Minimal Modbus RTU framing: just what is needed to read holding registers
// (function 0x03) and to write them (function 0x10; Deye inverters do not
// take 0x06). Pure functions, no I/O.
namespace modbus {

constexpr uint8_t kReadHoldingRegisters   = 0x03;
constexpr uint8_t kWriteMultipleRegisters = 0x10;

// Protocol limits for one request.
constexpr uint16_t kMaxReadQuantity  = 125;
constexpr uint16_t kMaxWriteQuantity = 123;

uint16_t crc16(const uint8_t* data, size_t len);

// slave | 0x03 | addr hi lo | qty hi lo | crc lo hi
std::vector<uint8_t> build_read_holding(uint8_t slave, uint16_t addr,
                                        uint16_t quantity);

// Validate a read-holding response against the request it answers and
// extract the register values. Handles the "double CRC" quirk of some
// loggers (two trailing zero bytes after a valid CRC). On failure returns
// false and fills `error` (a Modbus exception is reported by its code).
bool parse_read_holding(const std::vector<uint8_t>& frame, uint8_t slave,
                        uint16_t quantity, std::vector<uint16_t>& out,
                        std::string& error);

// slave | 0x10 | addr hi lo | qty hi lo | bytes | values hi lo ... | crc lo hi
std::vector<uint8_t> build_write_multiple(uint8_t slave, uint16_t addr,
                                          const std::vector<uint16_t>& values);

// The response echoes slave | 0x10 | addr | qty. Same error reporting and
// double-CRC handling as parse_read_holding.
bool parse_write_multiple(const std::vector<uint8_t>& frame, uint8_t slave,
                          uint16_t addr, uint16_t quantity, std::string& error);

}  // namespace modbus
