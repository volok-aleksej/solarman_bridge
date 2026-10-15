#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Solarman V5 framing (the TCP/8899 protocol of Solarman / IGEN WiFi data
// loggers). A V5 frame wraps a Modbus RTU frame:
//
//   A5 | len u16le | control u16le | seq u16le | logger serial u32le |
//   payload[len] | checksum u8 | 15
//
// The request payload is 15 bytes of header (frame type 0x02, sensor type,
// three zeroed u32 time fields) followed by the Modbus RTU request; the
// response payload is 14 bytes of header (frame type, status, three u32 time
// fields) followed by the Modbus RTU response. The checksum is the byte sum
// of everything between the start byte and the checksum itself.
//
// Mirrors pysolarmanv5's encoder/decoder. Pure functions, no I/O.
namespace v5 {

constexpr uint8_t  kStart           = 0xA5;
constexpr uint8_t  kEnd             = 0x15;
constexpr uint16_t kControlRequest  = 0x4510;
constexpr uint16_t kControlResponse = 0x1510;
// Unsolicited "counter"/heartbeat frames some loggers interleave.
constexpr uint16_t kControlCounter  = 0x4710;

constexpr size_t kHeaderLen  = 11;  // start .. logger serial
constexpr size_t kTrailerLen = 2;   // checksum + end
constexpr size_t kOverhead   = kHeaderLen + kTrailerLen;

uint8_t checksum(const uint8_t* frame, size_t len);

std::vector<uint8_t> encode_request(uint8_t seq, uint32_t logger_serial,
                                    const std::vector<uint8_t>& modbus_frame);

// Stream splitter for the receive buffer. Returns the size of the complete
// frame at the front of `buf`, 0 if more bytes are needed. Leading garbage
// (anything before a start byte) is reported via `junk` so the caller can
// drop it and rescan.
size_t frame_length(const std::vector<uint8_t>& buf, size_t& junk);

enum class DecodeResult {
    Ok,      // `modbus_out` holds the embedded Modbus RTU response
    Ignore,  // valid but not ours (counter frame, stale sequence number)
    Error,   // malformed; `error` says why
};

DecodeResult decode_response(const uint8_t* frame, size_t len, uint8_t seq,
                             uint32_t logger_serial,
                             std::vector<uint8_t>& modbus_out,
                             std::string& error);

}  // namespace v5
