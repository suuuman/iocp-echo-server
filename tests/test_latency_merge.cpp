//
//  분포 합치기 검증.
//
//  레인이 여럿일 때 백분위를 "가장 나쁜 레인의 값" 으로 대신하면
//  실제보다 비관적으로 나온다. 구간이 고정이므로 칸별로 더하면 정확해진다.
//
//  여기서 고정할 것은 "합친 분포가 처음부터 한 곳에 넣은 것과 같다" 는 것이다.
//
#include "core/latency.h"
#include "mini_check.h"

using core::LatencyHistogram;

TEST_CASE("빈 분포를 합쳐도 달라지지 않는다") {
    LatencyHistogram a;
    for (int i = 0; i < 10; ++i) a.add(50);

    const auto before = a.percentile(0.50);

    LatencyHistogram empty;
    a.merge(empty);

    CHECK_EQ(a.count(), static_cast<std::uint64_t>(10));
    CHECK_EQ(a.percentile(0.50), before);
}

TEST_CASE("표본 수가 더해진다") {
    LatencyHistogram a, b;
    for (int i = 0; i < 7; ++i) a.add(10);
    for (int i = 0; i < 3; ++i) b.add(20);

    a.merge(b);
    CHECK_EQ(a.count(), static_cast<std::uint64_t>(10));
}

TEST_CASE("최댓값은 더하지 않고 큰 쪽을 남긴다") {
    LatencyHistogram a, b;
    a.add(100);
    b.add(900);

    a.merge(b);
    CHECK_EQ(a.max(), static_cast<std::int64_t>(900));

    // 반대로 합쳐도 같아야 한다
    LatencyHistogram c, d;
    c.add(900);
    d.add(100);
    c.merge(d);
    CHECK_EQ(c.max(), static_cast<std::int64_t>(900));
}

TEST_CASE("나눠 넣고 합친 것이 한 곳에 넣은 것과 같다") {
    LatencyHistogram split_a, split_b, whole;

    // 한 쪽은 빠르고 다른 쪽은 느린, 실제로 레인이 갈렸을 때의 모습
    for (int i = 1; i <= 100; ++i) {
        split_a.add(i);
        whole.add(i);
    }
    for (int i = 1; i <= 100; ++i) {
        split_b.add(i * 50);
        whole.add(i * 50);
    }

    split_a.merge(split_b);

    CHECK_EQ(split_a.count(), whole.count());
    CHECK_EQ(split_a.max(), whole.max());
    CHECK_EQ(split_a.percentile(0.50), whole.percentile(0.50));
    CHECK_EQ(split_a.percentile(0.95), whole.percentile(0.95));
    CHECK_EQ(split_a.percentile(0.99), whole.percentile(0.99));
}

TEST_CASE("합친 백분위는 나쁜 쪽 값보다 낙관적이다") {
    // 느린 레인 하나와 빠른 레인 하나. 가장 나쁜 레인의 p50 을 쓰면
    // 전체의 절반이 그만큼 느린 것처럼 보인다
    LatencyHistogram fast, slow;
    for (int i = 0; i < 1000; ++i) fast.add(10);
    for (int i = 0; i < 1000; ++i) slow.add(5000);

    const auto worst_lane_p50 = slow.percentile(0.50);

    LatencyHistogram merged;
    merged.merge(fast);
    merged.merge(slow);

    CHECK(merged.percentile(0.50) < worst_lane_p50);
    CHECK_EQ(merged.count(), static_cast<std::uint64_t>(2000));
}

TEST_CASE("여러 번 합쳐도 누적된다") {
    LatencyHistogram total;
    for (int lane = 0; lane < 4; ++lane) {
        LatencyHistogram one;
        for (int i = 0; i < 25; ++i) one.add(100 + lane);
        total.merge(one);
    }
    CHECK_EQ(total.count(), static_cast<std::uint64_t>(100));
}
