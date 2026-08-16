#pragma once
//
//  단일 로직 스레드.
//
//  게임 상태를 이 스레드에서만 만진다. 그래서 상태에 잠금이 없다.
//  대신 처리량 상한이 이 스레드 하나에 걸린다. 감수하는 비용이다.
//
//  깨우는 방식을 주기 대기가 아니라 신호로 둔다.
//  Windows 의 기본 타이머 분해능은 15.625ms 라서 1ms 를 요청해도 그만큼 잔다.
//  주기 대기로 두면 왕복 지연이 그 값에 묶인다. 실제로 그렇게 나왔다.
//
//  소속 검사는 Release 빌드에서도 살려 둔다.
//  다른 스레드에서 상태를 만지는 사고는 증상이 간헐적이라
//  부하 시험 중에 잡히지 않으면 잡을 기회가 없다.
//
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace core {

class LogicThread {
public:
    using Step  = std::function<void()>;
    using Timer = std::function<void()>;

    LogicThread() = default;
    ~LogicThread();

    LogicThread(const LogicThread&)            = delete;
    LogicThread& operator=(const LogicThread&) = delete;

    // step 은 회차마다 한 번 호출된다. 큐 소비를 여기서 한다.
    // max_idle_wait_ms 는 신호가 없을 때 타이머 확인을 위해 깨어나는 상한이다.
    // spin_us 는 잠들기 전에 회전하며 기다리는 시간이다 - 아래 설명 참조
    bool start(Step step, int max_idle_wait_ms = 50, int spin_us = 200);
    void stop();

    // 할 일이 생겼음을 알린다. 어느 스레드에서든 호출한다.
    // 이미 신호가 서 있으면 잠금 없이 돌아간다
    void wake();

    // 기동 전에 등록한다
    void add_timer(int interval_ms, Timer fn);

    bool on_logic_thread() const noexcept;
    // 위반 시 기록을 남기고 즉시 중단한다
    void assert_on_logic_thread(const char* where) const;

    bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    std::uint64_t tick_count() const noexcept {
        return ticks_.load(std::memory_order_relaxed);
    }

private:
    struct TimerSlot {
        int          interval_ms = 0;
        std::int64_t next_ms     = 0;
        Timer        fn;
    };

    void run();
    int  wait_budget_ms(std::int64_t now) const;

    Step                       step_;
    std::vector<TimerSlot>     timers_;
    std::thread                thread_;
    std::atomic<bool>          running_{false};
    std::atomic<bool>          signaled_{false};
    std::atomic<std::uint64_t> ticks_{0};
    std::thread::id            owner_{};
    int                        max_idle_wait_ms_ = 50;
    // 잠들었다 깨어나는 데 드는 시간이 메시지 하나 처리 시간보다 크다.
    // 요청이 끊길 듯 이어지는 구간에서는 잠깐 회전하는 쪽이 싸다
    int                        spin_us_ = 200;

    mutable std::mutex         mutex_;
    std::condition_variable    cv_;
};

} // namespace core
