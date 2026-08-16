#pragma once
//
//  연결 하나의 요청 속도 제한.
//
//  초당 rate 개씩 채우고 최대 burst 개까지 모아 둔다.
//  메시지 하나가 한 개를 쓴다. 남은 것이 없으면 그 메시지를 거절한다.
//
//  잠깐 몰리는 것과 계속 몰아치는 것을 나누기 위해 모아 두는 몫을 둔다.
//  평균만 보면 짧은 폭주를 막지 못하고, 순간값만 보면 정상 사용을 막는다.
//
//  연결마다 하나씩 두고 그 연결의 수신 경로에서만 만진다. 그래서 잠금이 없다.
//
#include <algorithm>
#include <cstdint>

namespace net {

class RateLimiter {
public:
    // rate_per_sec 가 0 이하면 제한하지 않는다
    void configure(int rate_per_sec, int burst) noexcept {
        rate_  = rate_per_sec > 0 ? static_cast<double>(rate_per_sec) : 0.0;
        burst_ = burst > 0 ? static_cast<double>(burst)
                           : static_cast<double>(std::max(1, rate_per_sec));
        // 처음에는 가득 찬 상태로 시작한다.
        // 비어서 시작하면 접속 직후의 정상 요청이 걸린다
        tokens_  = burst_;
        last_ms_ = 0;
    }

    bool enabled() const noexcept { return rate_ > 0.0; }

    bool allow(std::int64_t now_ms) noexcept {
        if (rate_ <= 0.0) return true;

        if (last_ms_ == 0) last_ms_ = now_ms;

        const std::int64_t elapsed = now_ms - last_ms_;
        if (elapsed > 0) {
            tokens_  = std::min(burst_, tokens_ + rate_ * (static_cast<double>(elapsed) / 1000.0));
            last_ms_ = now_ms;
        }

        if (tokens_ < 1.0) return false;
        tokens_ -= 1.0;
        return true;
    }

private:
    double       rate_    = 0.0;   // 초당 충전량
    double       burst_   = 0.0;   // 모아 둘 수 있는 최대치
    double       tokens_  = 0.0;
    std::int64_t last_ms_ = 0;
};

} // namespace net
