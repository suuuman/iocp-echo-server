#include "core/latency.h"

#include <algorithm>
#include <cstring>

namespace core {
namespace {

// 구간이 바뀌는 지점과 그 구간의 폭.
// 표를 두면 add 와 되돌리기가 같은 값을 보게 되어 둘이 어긋나지 않는다
struct Range {
    std::int64_t from;    // 이 구간이 담기 시작하는 값(µs)
    std::int64_t step;    // 칸 하나의 폭
    int          first;   // 이 구간의 첫 칸 번호
    int          slots;   // 칸 수
};

constexpr Range kRanges[] = {
    {       0,      1,   0, 100},
    {     100,     10, 100,  90},
    {    1000,    100, 190,  90},
    {   10000,   1000, 280,  90},
    {  100000,  10000, 370,  90},
    { 1000000, 100000, 460,  90},
};
constexpr int kRangeCount = static_cast<int>(sizeof(kRanges) / sizeof(kRanges[0]));

// 마지막 칸은 상한을 넘긴 값을 모은다
constexpr int kOverflow = 550;

} // namespace

int LatencyHistogram::bucket_of(std::int64_t us) noexcept {
    if (us <= 0) return 0;

    for (int i = kRangeCount - 1; i >= 0; --i) {
        const Range& r = kRanges[i];
        if (us < r.from) continue;

        const int offset = static_cast<int>((us - r.from) / r.step);
        if (offset >= r.slots) return kOverflow;   // 마지막 구간을 넘겼다
        return r.first + offset;
    }
    return 0;
}

std::int64_t LatencyHistogram::bucket_upper(int index) noexcept {
    if (index >= kOverflow) return kRanges[kRangeCount - 1].from
                                 + kRanges[kRangeCount - 1].step * kRanges[kRangeCount - 1].slots;

    for (int i = kRangeCount - 1; i >= 0; --i) {
        const Range& r = kRanges[i];
        if (index < r.first) continue;
        return r.from + static_cast<std::int64_t>(index - r.first + 1) * r.step;
    }
    return 0;
}

void LatencyHistogram::add(std::int64_t us) noexcept {
    if (us < 0) us = 0;

    ++buckets_[bucket_of(us)];
    ++count_;
    sum_us_ += static_cast<std::uint64_t>(us);
    max_us_ = std::max(max_us_, us);
}

std::int64_t LatencyHistogram::percentile(double p) const noexcept {
    if (count_ == 0) return 0;

    p = std::clamp(p, 0.0, 1.0);

    // 경계에서 한 칸 모자라지 않게 올림으로 잡는다
    std::uint64_t target = static_cast<std::uint64_t>(
        static_cast<double>(count_) * p + 0.5);
    if (target == 0) target = 1;
    if (target > count_) target = count_;

    std::uint64_t seen = 0;
    for (int i = 0; i < kBucketCount; ++i) {
        seen += buckets_[i];
        if (seen >= target) {
            // 실제 최댓값보다 큰 값을 내놓지 않는다
            return std::min(bucket_upper(i), max_us_);
        }
    }
    return max_us_;
}

void LatencyHistogram::reset() noexcept {
    std::memset(buckets_, 0, sizeof(buckets_));
    count_  = 0;
    sum_us_ = 0;
    max_us_ = 0;
}

void LatencyHistogram::merge(const LatencyHistogram& other) noexcept {
    for (int i = 0; i < kBucketCount; ++i) buckets_[i] += other.buckets_[i];

    count_  += other.count_;
    sum_us_ += other.sum_us_;
    // 최댓값은 더하는 것이 아니라 큰 쪽을 남긴다
    if (other.max_us_ > max_us_) max_us_ = other.max_us_;
}

} // namespace core
