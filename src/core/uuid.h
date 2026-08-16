#pragma once
//
//  세션 키 생성.
//
//  운영체제 API 나 외부 라이브러리를 쓰지 않는다. 필요한 것은
//  "충돌하지 않는 36자 식별자" 뿐이고, 그 조건은 128비트 난수로 충족된다.
//
//  스레드마다 독립된 생성기를 두어 잠금 없이 만든다.
//
#include <cstdint>
#include <random>
#include <string>

namespace core {

inline std::string make_session_key() {
    // 스레드 지역이라 경합이 없다. 시드는 스레드당 한 번만 뽑는다
    static thread_local std::mt19937_64 rng{std::random_device{}()};

    const std::uint64_t hi = rng();
    const std::uint64_t lo = rng();

    static constexpr char kHex[] = "0123456789abcdef";

    // 8-4-4-4-12 배치. 구분자는 8 · 13 · 18 · 23 네 곳에 그대로 남는다
    std::string out(36, '-');

    auto write_hex = [&out](std::size_t start, int digits, std::uint64_t value) {
        for (int i = 0; i < digits; ++i) {
            const int shift = (digits - 1 - i) * 4;
            out[start + static_cast<std::size_t>(i)] =
                kHex[(value >> shift) & 0xF];
        }
    };

    write_hex(0,   8, hi >> 32);
    write_hex(9,   4, (hi >> 16) & 0xFFFF);
    write_hex(14,  4, 0x4000 | (hi & 0x0FFF));          // 버전 4 표시
    write_hex(19,  4, 0x8000 | ((lo >> 48) & 0x3FFF));  // 변형 표시
    write_hex(24, 12, lo & 0xFFFFFFFFFFFFull);

    return out;
}

} // namespace core
