#pragma once
//
//  시간 원천을 두 종류로 나눈다.
//    now_ms()  - 단조 증가. 타임아웃 · 주기 판정에 쓴다.
//                시스템 시각이 바뀌어도 영향받지 않는다
//    unix_ms() - 벽시계. 기록 · 외부 전달에 쓴다
//
#include <chrono>
#include <cstdint>

namespace core {

inline std::int64_t now_ms() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// 큐 대기 시간처럼 밀리초로는 분해되지 않는 구간에 쓴다
inline std::int64_t now_us() noexcept {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// 메시지 하나를 처리하는 시간은 마이크로초 미만이라 그 단위로는 전부 0 이 된다.
// 분포를 보려면 그보다 잘게 나뉘어야 한다
inline std::int64_t now_ns() noexcept {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

inline std::int64_t unix_ms() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

} // namespace core
