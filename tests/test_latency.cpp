//
//  지연 분포 검증.
//
//  구간으로 뭉개는 자료구조라 값이 정확히 맞지는 않는다.
//  지켜야 하는 것은 "구간 안에 들어온다" 와 "순서가 뒤집히지 않는다" 두 가지다.
//
#include "core/latency.h"
#include "mini_check.h"

using core::LatencyHistogram;

TEST_CASE("표본이 없으면 0 이다") {
    LatencyHistogram h;
    CHECK_EQ(h.count(), static_cast<std::uint64_t>(0));
    CHECK_EQ(h.percentile(0.5), static_cast<std::int64_t>(0));
    CHECK_EQ(h.max(), static_cast<std::int64_t>(0));
}

TEST_CASE("구간 번호가 값에 따라 커진다") {
    CHECK_EQ(LatencyHistogram::bucket_of(0), 0);
    CHECK_EQ(LatencyHistogram::bucket_of(1), 1);
    CHECK_EQ(LatencyHistogram::bucket_of(99), 99);
    CHECK_EQ(LatencyHistogram::bucket_of(100), 100);      // 10µs 구간 시작
    CHECK_EQ(LatencyHistogram::bucket_of(1000), 190);     // 100µs 구간 시작
    CHECK_EQ(LatencyHistogram::bucket_of(10000), 280);    // 1ms 구간 시작
    CHECK_EQ(LatencyHistogram::bucket_of(100000), 370);   // 10ms 구간 시작
    CHECK_EQ(LatencyHistogram::bucket_of(1000000), 460);  // 100ms 구간 시작

    // 상한을 넘긴 값은 한 칸에 모인다
    CHECK_EQ(LatencyHistogram::bucket_of(10000000), 550);
    CHECK_EQ(LatencyHistogram::bucket_of(999999999), 550);
}

TEST_CASE("같은 값만 넣으면 백분위가 그 값이다") {
    LatencyHistogram h;
    for (int i = 0; i < 1000; ++i) h.add(50);

    CHECK_EQ(h.count(), static_cast<std::uint64_t>(1000));
    CHECK_EQ(h.max(), static_cast<std::int64_t>(50));
    CHECK_EQ(h.percentile(0.50), static_cast<std::int64_t>(50));
    CHECK_EQ(h.percentile(0.99), static_cast<std::int64_t>(50));
}

TEST_CASE("백분위가 값의 순서를 따른다") {
    LatencyHistogram h;
    for (int i = 1; i <= 100; ++i) h.add(i);   // 1 ~ 100µs 고르게

    const auto p50 = h.percentile(0.50);
    const auto p95 = h.percentile(0.95);
    const auto p99 = h.percentile(0.99);

    CHECK(p50 <= p95);
    CHECK(p95 <= p99);

    // 1µs 폭 구간이라 이 범위에서는 거의 정확하다
    CHECK(p50 >= 49 && p50 <= 52);
    CHECK(p95 >= 94 && p95 <= 97);
    CHECK_EQ(h.max(), static_cast<std::int64_t>(100));
}

TEST_CASE("드물게 튀는 값이 상단에 잡힌다") {
    LatencyHistogram h;
    for (int i = 0; i < 990; ++i) h.add(10);    // 대부분 10µs
    for (int i = 0; i < 10; ++i)  h.add(50000); // 1% 가 50ms

    // 평균은 두 값 사이 어딘가로 뭉개진다
    CHECK(h.mean() > 10.0);
    CHECK(h.mean() < 1000.0);

    // 중앙값은 여전히 짧지만 상단은 튄 값을 가리킨다.
    // 값은 구간 상한으로 보고되므로 10µs 표본은 10~11 사이로 나온다
    const auto p50 = h.percentile(0.50);
    CHECK(p50 >= 10 && p50 <= 11);
    CHECK(h.percentile(0.999) >= 40000);
    CHECK_EQ(h.max(), static_cast<std::int64_t>(50000));
}

TEST_CASE("백분위가 실제 최댓값을 넘지 않는다") {
    LatencyHistogram h;
    h.add(1500);   // 100µs 폭 구간에 들어간다

    // 구간 상한은 1600 이지만 실제 최댓값을 넘겨 보고하지 않는다
    CHECK_EQ(h.percentile(1.0), static_cast<std::int64_t>(1500));
    CHECK_EQ(h.max(), static_cast<std::int64_t>(1500));
}

TEST_CASE("음수는 0 으로 본다") {
    LatencyHistogram h;
    h.add(-5);
    CHECK_EQ(h.count(), static_cast<std::uint64_t>(1));
    CHECK_EQ(h.max(), static_cast<std::int64_t>(0));
}

TEST_CASE("초기화하면 표본이 사라진다") {
    LatencyHistogram h;
    for (int i = 0; i < 100; ++i) h.add(i);
    h.reset();

    CHECK_EQ(h.count(), static_cast<std::uint64_t>(0));
    CHECK_EQ(h.max(), static_cast<std::int64_t>(0));
    CHECK_EQ(h.percentile(0.5), static_cast<std::int64_t>(0));

    // 초기화 뒤에도 정상 동작한다
    h.add(7);
    CHECK_EQ(h.percentile(0.5), static_cast<std::int64_t>(7));
}
