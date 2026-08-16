#include "core/logic_thread.h"

#include <algorithm>
#include <cstdlib>

#include "core/clock.h"
#include "core/log.h"

namespace core {

LogicThread::~LogicThread() {
    stop();
}

bool LogicThread::start(Step step, int max_idle_wait_ms, int spin_us) {
    if (running()) return true;
    if (!step) return false;

    step_             = std::move(step);
    max_idle_wait_ms_ = std::max(1, max_idle_wait_ms);
    spin_us_          = std::max(0, spin_us);

    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { run(); });

    // 소속 검사가 성립하려면 owner_ 가 먼저 정해져 있어야 한다.
    // run() 진입부에서 기록하므로 여기서 잠시 기다린다
    while (owner_ == std::thread::id{} && running()) {
        std::this_thread::yield();
    }

    LOG_INFO("logic thread started timers=%zu", timers_.size());
    return true;
}

void LogicThread::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;

    // 대기 중이면 깨워서 종료 조건을 다시 보게 한다
    {
        std::lock_guard<std::mutex> guard(mutex_);
        signaled_.store(true, std::memory_order_release);
    }
    cv_.notify_all();

    if (thread_.joinable()) thread_.join();
    owner_ = std::thread::id{};

    LOG_INFO("logic thread stopped ticks=%llu",
             static_cast<unsigned long long>(ticks_.load()));
}

void LogicThread::wake() {
    // 신호가 이미 서 있으면 잠금을 잡지 않는다.
    // 부하 구간에서는 대부분 이 경로로 빠진다
    if (signaled_.exchange(true, std::memory_order_release)) return;

    {
        std::lock_guard<std::mutex> guard(mutex_);
    }
    cv_.notify_one();
}

void LogicThread::add_timer(int interval_ms, Timer fn) {
    if (interval_ms <= 0 || !fn) return;
    timers_.push_back(TimerSlot{interval_ms, 0, std::move(fn)});
}

bool LogicThread::on_logic_thread() const noexcept {
    return std::this_thread::get_id() == owner_;
}

void LogicThread::assert_on_logic_thread(const char* where) const {
    if (on_logic_thread()) return;

    // 여기까지 왔다는 것은 상태를 다른 스레드에서 만졌다는 뜻이다.
    // 계속 돌면 손상된 상태로 동작하므로 그 자리에서 멈춘다
    LOG_ERROR("thread affinity violation at %s", where ? where : "?");
    log_stop();
    std::abort();
}

int LogicThread::wait_budget_ms(std::int64_t now) const {
    int budget = max_idle_wait_ms_;
    for (const auto& t : timers_) {
        const auto remain = static_cast<int>(std::max<std::int64_t>(0, t.next_ms - now));
        budget = std::min(budget, remain);
    }
    return budget;
}

void LogicThread::run() {
    owner_ = std::this_thread::get_id();

    const std::int64_t start_ms = now_ms();
    for (auto& t : timers_) t.next_ms = start_ms + t.interval_ms;

    while (running()) {
        // 먼저 내린다. step 도중에 들어온 항목은 다시 신호를 세운다
        signaled_.store(false, std::memory_order_release);

        step_();

        const std::int64_t current = now_ms();
        for (auto& t : timers_) {
            if (current >= t.next_ms) {
                t.next_ms = current + t.interval_ms;
                t.fn();
            }
        }

        ticks_.fetch_add(1, std::memory_order_relaxed);

        if (signaled_.load(std::memory_order_acquire) || !running()) continue;

        // 잠들기 전에 잠깐 회전한다.
        // 문맥 전환 왕복이 메시지 하나 처리보다 비싸므로,
        // 요청이 곧 이어질 구간에서는 깨우기 비용을 그대로 지연으로 물게 된다
        if (spin_us_ > 0) {
            const std::int64_t spin_until = now_us() + spin_us_;
            while (now_us() < spin_until) {
                if (signaled_.load(std::memory_order_acquire) ||
                    !running_.load(std::memory_order_acquire)) {
                    break;
                }
                std::this_thread::yield();
            }
            if (signaled_.load(std::memory_order_acquire) || !running()) continue;
        }

        const int budget = wait_budget_ms(now_ms());
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(budget), [this] {
            return signaled_.load(std::memory_order_acquire) ||
                   !running_.load(std::memory_order_acquire);
        });
    }

    // 종료 직전에 한 번 더 돌려 남은 항목을 소비한다
    step_();
}

} // namespace core
