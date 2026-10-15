#include "solarman_v5.hpp"

#include <algorithm>

namespace v5 {

namespace {

constexpr uint8_t kFrameTypeInverter = 0x02;
constexpr size_t  kRequestPayloadHdr  = 15;
constexpr size_t  kResponsePayloadHdr = 14;

uint16_t get_u16le(const uint8_t* p) { return p[0] | (p[1] << 8); }

uint32_t get_u32le(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t{p[3]} << 24);
}

void put_u16le(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(x & 0xFF);
    v.push_back(x >> 8);
}

void put_u32le(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xFF);
}

}  // namespace

uint8_t checksum(const uint8_t* frame, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 1; i + 2 < len; ++i) sum += frame[i];
    return sum;
}

std::vector<uint8_t> encode_request(uint8_t seq, uint32_t logger_serial,
                                    const std::vector<uint8_t>& modbus_frame) {
    std::vector<uint8_t> f;
    f.reserve(kOverhead + kRequestPayloadHdr + modbus_frame.size());

    f.push_back(kStart);
    put_u16le(f, kRequestPayloadHdr + modbus_frame.size());
    put_u16le(f, kControlRequest);
    put_u16le(f, seq);
    put_u32le(f, logger_serial);

    f.push_back(kFrameTypeInverter);
    put_u16le(f, 0);  // sensor type
    put_u32le(f, 0);  // total working time
    put_u32le(f, 0);  // power-on time
    put_u32le(f, 0);  // offset time
    f.insert(f.end(), modbus_frame.begin(), modbus_frame.end());

    f.push_back(0);   // checksum placeholder
    f.push_back(kEnd);
    f[f.size() - 2] = checksum(f.data(), f.size());
    return f;
}

size_t frame_length(const std::vector<uint8_t>& buf, size_t& junk) {
    auto start = std::find(buf.begin(), buf.end(), kStart);
    junk = start - buf.begin();
    if (junk) return 0;
    if (buf.size() < 3) return 0;
    const size_t total = kOverhead + get_u16le(&buf[1]);
    return buf.size() >= total ? total : 0;
}

DecodeResult decode_response(const uint8_t* frame, size_t len, uint8_t seq,
                             uint32_t logger_serial,
                             std::vector<uint8_t>& modbus_out,
                             std::string& error) {
    if (len < kOverhead || frame[0] != kStart || frame[len - 1] != kEnd) {
        error = "V5 frame has invalid start/end bytes";
        return DecodeResult::Error;
    }
    if (len != kOverhead + get_u16le(frame + 1)) {
        error = "V5 frame length does not match its length field";
        return DecodeResult::Error;
    }
    if (frame[len - 2] != checksum(frame, len)) {
        error = "V5 frame checksum mismatch";
        return DecodeResult::Error;
    }

    const uint16_t control = get_u16le(frame + 3);
    if (control == kControlCounter) return DecodeResult::Ignore;
    // Only the low byte of the sequence field is echoed back.
    if (frame[5] != seq) return DecodeResult::Ignore;

    if (control != kControlResponse) {
        error = "V5 frame has unexpected control code";
        return DecodeResult::Error;
    }
    if (get_u32le(frame + 7) != logger_serial) {
        error = "V5 frame carries a different logger serial";
        return DecodeResult::Error;
    }
    if (frame[kHeaderLen] != kFrameTypeInverter) {
        error = "V5 frame has unexpected frame type";
        return DecodeResult::Error;
    }

    const size_t mb_begin = kHeaderLen + kResponsePayloadHdr;
    const size_t mb_end   = len - kTrailerLen;
    if (mb_end < mb_begin + 5) {
        // The logger answers this way when the inverter did not respond on
        // its RS485 side.
        error = "no Modbus response (inverter not answering?)";
        return DecodeResult::NoReply;
    }
    modbus_out.assign(frame + mb_begin, frame + mb_end);
    return DecodeResult::Ok;
}

}  // namespace v5
