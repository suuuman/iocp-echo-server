#pragma once
//
//  본문 역직렬화.
//
//  모든 읽기는 남은 길이를 먼저 확인한다.
//  한 번이라도 범위를 벗어나면 ok_ 가 내려가고 이후 읽기는 전부 무시된다.
//  호출부는 필드마다 검사하지 않고 마지막에 ok() 한 번만 보면 된다.
//
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "proto/byte_order.h"

namespace proto {

class ByteReader {
public:
    ByteReader(const char* data, std::uint32_t size) noexcept
        : data_(data), size_(size) {}

    std::uint8_t u8() {
        if (!need(1)) return 0;
        return static_cast<std::uint8_t>(data_[pos_++]);
    }
    std::uint16_t u16() {
        if (!need(2)) return 0;
        const auto v = load_be16(data_ + pos_); pos_ += 2; return v;
    }
    std::uint32_t u32() {
        if (!need(4)) return 0;
        const auto v = load_be32(data_ + pos_); pos_ += 4; return v;
    }
    std::uint64_t u64() {
        if (!need(8)) return 0;
        const auto v = load_be64(data_ + pos_); pos_ += 8; return v;
    }
    std::int64_t i64() { return static_cast<std::int64_t>(u64()); }

    // 반환 문자열은 원본 버퍼를 가리킨다. 수신 버퍼 수명 안에서만 유효하다
    std::string_view str() {
        const std::uint32_t n = u16();
        if (!ok_ || !need(n)) return {};
        std::string_view v(data_ + pos_, n);
        pos_ += n;
        return v;
    }

    bool ok() const noexcept { return ok_; }
    // 남은 바이트가 없어야 정상이다. 남아 있으면 형식이 어긋난 것이다
    bool consumed_all() const noexcept { return ok_ && pos_ == size_; }
    std::uint32_t remaining() const noexcept { return ok_ ? size_ - pos_ : 0; }

private:
    bool need(std::uint32_t n) noexcept {
        if (!ok_) return false;
        if (size_ - pos_ < n) { ok_ = false; return false; }
        return true;
    }

    const char*   data_ = nullptr;
    std::uint32_t size_ = 0;
    std::uint32_t pos_  = 0;
    bool          ok_   = true;
};

} // namespace proto
