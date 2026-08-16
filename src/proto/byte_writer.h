#pragma once
//
//  본문 직렬화. 헤더는 프레임 계층이 붙인다.
//
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "proto/byte_order.h"

namespace proto {

class ByteWriter {
public:
    explicit ByteWriter(std::size_t reserve = 64) { buf_.reserve(reserve); }

    void u8(std::uint8_t v)   { buf_.push_back(static_cast<char>(v)); }
    void u16(std::uint16_t v) { char t[2]; store_be16(t, v); append(t, 2); }
    void u32(std::uint32_t v) { char t[4]; store_be32(t, v); append(t, 4); }
    void u64(std::uint64_t v) { char t[8]; store_be64(t, v); append(t, 8); }
    void i64(std::int64_t v)  { u64(static_cast<std::uint64_t>(v)); }

    // u16 바이트 길이 + UTF-8 바이트열. 널 종료를 붙이지 않는다
    void str(std::string_view s) {
        const auto n = static_cast<std::uint16_t>(
            s.size() > 0xFFFF ? 0xFFFF : s.size());
        u16(n);
        append(s.data(), n);
    }

    const char*  data() const noexcept { return buf_.data(); }
    std::uint32_t size() const noexcept { return static_cast<std::uint32_t>(buf_.size()); }
    void clear() noexcept { buf_.clear(); }

private:
    void append(const char* p, std::size_t n) {
        if (n == 0) return;
        buf_.insert(buf_.end(), p, p + n);
    }

    std::vector<char> buf_;
};

} // namespace proto
