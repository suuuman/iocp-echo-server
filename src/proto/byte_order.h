#pragma once
//
//  네트워크 바이트 순서(big-endian) 변환.
//  ntohl 계열을 쓰지 않고 직접 둔 이유 -
//    ① winsock 헤더 의존 없이 프로토콜 계층만 단위 테스트할 수 있다
//    ② 정렬되지 않은 주소에서도 안전하다. 수신 버퍼의 메시지 시작점은
//       4바이트 경계에 있다는 보장이 없다
//
#include <cstdint>
#include <cstring>

namespace proto {

inline std::uint16_t load_be16(const void* p) noexcept {
    const auto* b = static_cast<const std::uint8_t*>(p);
    return static_cast<std::uint16_t>(b[0]) << 8 |
           static_cast<std::uint16_t>(b[1]);
}

inline std::uint32_t load_be32(const void* p) noexcept {
    const auto* b = static_cast<const std::uint8_t*>(p);
    return static_cast<std::uint32_t>(b[0]) << 24 |
           static_cast<std::uint32_t>(b[1]) << 16 |
           static_cast<std::uint32_t>(b[2]) << 8  |
           static_cast<std::uint32_t>(b[3]);
}

inline std::uint64_t load_be64(const void* p) noexcept {
    const auto* b = static_cast<const std::uint8_t*>(p);
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | b[i];
    return v;
}

inline void store_be16(void* p, std::uint16_t v) noexcept {
    auto* b = static_cast<std::uint8_t*>(p);
    b[0] = static_cast<std::uint8_t>(v >> 8);
    b[1] = static_cast<std::uint8_t>(v);
}

inline void store_be32(void* p, std::uint32_t v) noexcept {
    auto* b = static_cast<std::uint8_t*>(p);
    b[0] = static_cast<std::uint8_t>(v >> 24);
    b[1] = static_cast<std::uint8_t>(v >> 16);
    b[2] = static_cast<std::uint8_t>(v >> 8);
    b[3] = static_cast<std::uint8_t>(v);
}

inline void store_be64(void* p, std::uint64_t v) noexcept {
    auto* b = static_cast<std::uint8_t*>(p);
    for (int i = 7; i >= 0; --i) { b[i] = static_cast<std::uint8_t>(v); v >>= 8; }
}

} // namespace proto
