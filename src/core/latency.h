#pragma once
//
//  지연 분포.
//
//  평균만으로는 "가끔 튀는 요청" 을 볼 수 없다.
//  평균이 좋아 보이는 구간에서도 백분위 상단은 그 수십 배인 경우가 흔하다.
//
//  표본을 모두 들고 있으면 요청 수에 비례해 메모리가 는다.
//  구간별 개수만 세면 요청이 몇 건이든 크기가 고정된다.
//  대신 값이 구간 폭만큼 뭉개진다 - 백분위를 보는 목적에는 그 정도면 된다.
//
//  구간은 아래로 갈수록 넓다. 짧은 쪽을 촘촘히 보는 편이 쓸모 있고,
//  긴 쪽은 "몇 밀리초대인가" 만 알면 되기 때문이다.
//
//      0 ~ 99µs       1µs 단위      100칸
//      100µs ~ 1ms    10µs 단위      90칸
//      1ms ~ 10ms     100µs 단위     90칸
//      10ms ~ 100ms   1ms 단위       90칸
//      100ms ~ 1s     10ms 단위      90칸
//      1s ~ 10s       100ms 단위     90칸
//      10s 이상       한 칸에 모은다
//
//  한 스레드에서만 쓴다. 그래서 잠금이 없다.
//
#include <cstdint>

namespace core {

class LatencyHistogram {
public:
    static constexpr int kBucketCount = 551;

    void add(std::int64_t us) noexcept;

    // p 는 0.0 ~ 1.0. 값이 없으면 0.
    // 해당 표본이 들어간 구간의 **상한**을 돌려준다. 실제 최댓값은 넘지 않는다
    std::int64_t percentile(double p) const noexcept;

    std::uint64_t count() const noexcept { return count_; }
    std::int64_t  max() const noexcept   { return max_us_; }
    double        mean() const noexcept {
        return count_ ? static_cast<double>(sum_us_) / static_cast<double>(count_) : 0.0;
    }

    void reset() noexcept;

    // 다른 분포를 흡수한다.
    //
    // 서로 다른 분포의 백분위는 더하거나 평균 낼 수 없다. 대신 분포 자체는 합칠 수 있다 -
    // 구간이 고정이므로 칸별로 더하면 그대로 합쳐진 분포가 된다.
    // 그래야 여러 레인의 값을 하나의 백분위로 낼 수 있다.
    //
    // 같은 종류의 일을 잰 분포끼리만 합쳐야 한다.
    // 성격이 다른 구간을 합치면 그 결과는 어느 쪽도 설명하지 못한다
    void merge(const LatencyHistogram& other) noexcept;

    // 구간 하나의 상한값(µs). 백분위를 되돌릴 때 쓴다
    static std::int64_t bucket_upper(int index) noexcept;
    static int          bucket_of(std::int64_t us) noexcept;

private:
    std::uint64_t buckets_[kBucketCount]{};
    std::uint64_t count_   = 0;
    std::uint64_t sum_us_  = 0;
    std::int64_t  max_us_  = 0;
};

} // namespace core
