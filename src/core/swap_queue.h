#pragma once
//
//  이중 버퍼 큐. 생산자 여럿, 소비자 하나.
//
//  버퍼 두 개를 두고 적재 대상만 바꾼다.
//  잠금 구간이 "항목 하나 넣기" 또는 "인덱스 교체" 로 끝나므로
//  소비자가 항목을 처리하는 동안 생산자가 대기하지 않는다.
//
//  소비자는 swap() 이 돌려준 버퍼를 다음 swap() 전까지만 사용한다.
//
#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

namespace core {

template <typename T>
class SwapQueue {
public:
    SwapQueue() = default;

    SwapQueue(const SwapQueue&)            = delete;
    SwapQueue& operator=(const SwapQueue&) = delete;

    // 생산자. 어느 스레드에서든 호출한다
    void push(T&& item) {
        std::lock_guard<std::mutex> guard(mutex_);
        buffers_[write_].push_back(std::move(item));
    }

    // 항목이 큰 경우에 쓴다.
    // 밖에서 만들어 넘기면 이동 한 번이 통째로 복사가 되므로
    // 큐 안에 자리를 잡고 그 자리에 직접 채운다.
    // 채우는 동안 잠금을 쥔다 - 다른 생산자가 버퍼를 늘려 참조를 무효화할 수 있어서다
    template <typename Fill>
    void emplace(Fill&& fill) {
        std::lock_guard<std::mutex> guard(mutex_);
        fill(buffers_[write_].emplace_back());
    }

    // 상한이 있는 적재. 적재 대상이 이미 cap 이면 채우지 않고 false 를 돌려준다.
    // 소비가 밀리는 동안에도 생산은 계속되므로, 상한이 없으면 그 차이가
    // 그대로 메모리 증가가 된다. 거절 여부는 호출부가 판단한다
    template <typename Fill>
    bool try_emplace(std::size_t cap, Fill&& fill) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (buffers_[write_].size() >= cap) return false;
        fill(buffers_[write_].emplace_back());
        return true;
    }

    // 소비자 전용. 채워진 쪽을 돌려주고 적재 대상을 반대편으로 바꾼다.
    // 반환 참조는 다음 swap() 호출 전까지 유효하다
    std::vector<T>& swap() {
        std::lock_guard<std::mutex> guard(mutex_);
        std::vector<T>& filled = buffers_[write_];
        write_ ^= 1;
        // 새 적재 대상은 지난 회차에 소비가 끝난 쪽이다
        buffers_[write_].clear();
        return filled;
    }

    std::size_t pending() {
        std::lock_guard<std::mutex> guard(mutex_);
        return buffers_[write_].size();
    }

    // 소비자가 미리 자리를 잡아 재할당을 줄인다
    void reserve(std::size_t n) {
        std::lock_guard<std::mutex> guard(mutex_);
        buffers_[0].reserve(n);
        buffers_[1].reserve(n);
    }

private:
    std::mutex     mutex_;
    std::vector<T> buffers_[2];
    int            write_ = 0;
};

} // namespace core
