//
//  요청 속도 제한 검증.
//
//  시간을 인자로 받게 만들어 두었으므로 실제로 기다리지 않고 검증한다.
//  잠들면서 재는 시험은 느리고, 장비가 바쁠 때 결과가 흔들린다.
//
#include "mini_check.h"
#include "net/rate_limiter.h"

using net::RateLimiter;

TEST_CASE("설정하지 않으면 제한하지 않는다") {
    RateLimiter r;
    CHECK(!r.enabled());
    for (int i = 0; i < 1000; ++i) CHECK(r.allow(0));
}

TEST_CASE("상한을 0 으로 주면 제한하지 않는다") {
    RateLimiter r;
    r.configure(0, 0);
    CHECK(!r.enabled());
    for (int i = 0; i < 1000; ++i) CHECK(r.allow(0));
}

TEST_CASE("모아 둔 몫까지는 한 번에 통과한다") {
    RateLimiter r;
    r.configure(10, 5);   // 초당 10개, 최대 5개 보관
    CHECK(r.enabled());

    // 가득 찬 상태로 시작한다
    for (int i = 0; i < 5; ++i) CHECK(r.allow(1000));
    CHECK(!r.allow(1000));   // 여섯 번째는 막힌다
}

TEST_CASE("시간이 지나면 다시 채워진다") {
    RateLimiter r;
    r.configure(10, 5);

    for (int i = 0; i < 5; ++i) CHECK(r.allow(1000));
    CHECK(!r.allow(1000));

    // 100ms 뒤면 1개가 채워진다 (초당 10개)
    CHECK(r.allow(1100));
    CHECK(!r.allow(1100));

    // 500ms 뒤면 5개 - 보관 한도까지만 찬다
    for (int i = 0; i < 5; ++i) CHECK(r.allow(1600));
    CHECK(!r.allow(1600));
}

TEST_CASE("오래 쉬어도 보관 한도를 넘지 않는다") {
    RateLimiter r;
    r.configure(10, 5);
    for (int i = 0; i < 5; ++i) CHECK(r.allow(1000));

    // 한 시간을 쉬어도 5개까지만 모인다.
    // 이 한도가 없으면 쉬었다가 한꺼번에 쏟는 것을 막지 못한다
    for (int i = 0; i < 5; ++i) CHECK(r.allow(1000 + 3600 * 1000));
    CHECK(!r.allow(1000 + 3600 * 1000));
}

TEST_CASE("보관 몫을 주지 않으면 상한과 같게 둔다") {
    RateLimiter r;
    r.configure(3, 0);
    for (int i = 0; i < 3; ++i) CHECK(r.allow(500));
    CHECK(!r.allow(500));
}

TEST_CASE("꾸준한 속도는 막지 않는다") {
    RateLimiter r;
    r.configure(100, 10);   // 초당 100개

    // 10ms 마다 한 건이면 초당 100건이다. 계속 통과해야 한다
    std::int64_t now = 0;
    int passed = 0;
    for (int i = 0; i < 200; ++i) {
        if (r.allow(now)) ++passed;
        now += 10;
    }
    CHECK_EQ(passed, 200);
}

TEST_CASE("상한을 넘는 속도는 상한만큼만 통과한다") {
    RateLimiter r;
    r.configure(100, 10);

    // 1ms 마다 한 건 = 초당 1000건. 상한의 10배다
    std::int64_t now = 0;
    int passed = 0;
    for (int i = 0; i < 1000; ++i) {
        if (r.allow(now)) ++passed;
        now += 1;
    }

    // 1초 동안 보관분 10 + 충전 100 안팎이 통과한다
    CHECK(passed >= 100);
    CHECK(passed <= 120);
}
