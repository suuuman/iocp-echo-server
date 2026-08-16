//
//  이중 버퍼 큐 검증.
//
//  생산자 여럿과 소비자 하나가 동시에 도는 구조라
//  "항목을 잃지 않는다" 와 "중복되지 않는다" 두 가지를 고정한다.
//
#include <atomic>
#include <thread>
#include <unordered_set>
#include <vector>

#include "core/swap_queue.h"
#include "mini_check.h"

using core::SwapQueue;

TEST_CASE("빈 큐를 교체하면 빈 버퍼가 나온다") {
    SwapQueue<int> q;
    CHECK(q.swap().empty());
    CHECK_EQ(q.pending(), static_cast<std::size_t>(0));
}

TEST_CASE("적재한 순서대로 나온다") {
    SwapQueue<int> q;
    for (int i = 0; i < 5; ++i) q.push(std::move(i));

    auto& batch = q.swap();
    CHECK_EQ(batch.size(), static_cast<std::size_t>(5));
    for (int i = 0; i < 5; ++i) CHECK_EQ(batch[i], i);
}

TEST_CASE("교체 후에는 적재 대상이 바뀐다") {
    SwapQueue<int> q;
    q.push(1);

    auto& first = q.swap();
    CHECK_EQ(first.size(), static_cast<std::size_t>(1));

    // 소비 중에 들어온 항목은 다음 회차에 나온다
    q.push(2);
    CHECK_EQ(first.size(), static_cast<std::size_t>(1));   // 처리 중 버퍼는 그대로다

    auto& second = q.swap();
    CHECK_EQ(second.size(), static_cast<std::size_t>(1));
    CHECK_EQ(second[0], 2);
}

TEST_CASE("이동 전용 타입을 담는다") {
    SwapQueue<std::vector<int>> q;
    std::vector<int> v{1, 2, 3};
    q.push(std::move(v));

    auto& batch = q.swap();
    CHECK_EQ(batch.size(), static_cast<std::size_t>(1));
    CHECK_EQ(batch[0].size(), static_cast<std::size_t>(3));
}

TEST_CASE("상한에 닿으면 적재를 거절한다") {
    SwapQueue<int> q;

    for (int i = 0; i < 3; ++i) {
        CHECK(q.try_emplace(3, [i](int& slot) { slot = i; }));
    }
    // 세 자리가 찼다. 네 번째는 들어가지 않는다
    CHECK(!q.try_emplace(3, [](int& slot) { slot = 99; }));
    CHECK_EQ(q.pending(), static_cast<std::size_t>(3));

    auto& batch = q.swap();
    CHECK_EQ(batch.size(), static_cast<std::size_t>(3));
    CHECK_EQ(batch[2], 2);   // 거절된 항목이 섞이지 않았다
}

TEST_CASE("교체로 자리가 나면 다시 적재된다") {
    SwapQueue<int> q;
    CHECK(q.try_emplace(1, [](int& slot) { slot = 1; }));
    CHECK(!q.try_emplace(1, [](int& slot) { slot = 2; }));

    q.swap();   // 적재 대상이 빈 쪽으로 바뀐다

    CHECK(q.try_emplace(1, [](int& slot) { slot = 3; }));
    CHECK_EQ(q.pending(), static_cast<std::size_t>(1));
}

TEST_CASE("상한이 0 이면 아무것도 들어가지 않는다") {
    SwapQueue<int> q;
    CHECK(!q.try_emplace(0, [](int& slot) { slot = 1; }));
    CHECK_EQ(q.pending(), static_cast<std::size_t>(0));
}

TEST_CASE("생산자 여럿이 넣어도 잃거나 겹치지 않는다") {
    constexpr int kProducers = 4;
    constexpr int kPerThread = 5000;

    SwapQueue<int>    q;
    std::atomic<bool> done{false};
    std::unordered_set<int> seen;
    seen.reserve(kProducers * kPerThread);

    std::vector<std::thread> producers;
    for (int t = 0; t < kProducers; ++t) {
        producers.emplace_back([&q, t] {
            for (int i = 0; i < kPerThread; ++i) {
                int value = t * kPerThread + i;   // 전 구간에서 유일한 값
                q.push(std::move(value));
            }
        });
    }

    // 소비자는 하나다. 생산이 끝난 뒤에도 한 번 더 비운다
    std::thread consumer([&] {
        for (;;) {
            const bool finished = done.load(std::memory_order_acquire);
            auto& batch = q.swap();
            for (int v : batch) seen.insert(v);
            batch.clear();
            if (finished && q.pending() == 0) break;
            std::this_thread::yield();
        }
    });

    for (auto& t : producers) t.join();
    done.store(true, std::memory_order_release);
    consumer.join();

    // 마지막 잔여분까지 비운다
    auto& tail = q.swap();
    for (int v : tail) seen.insert(v);

    CHECK_EQ(seen.size(), static_cast<std::size_t>(kProducers * kPerThread));
}
