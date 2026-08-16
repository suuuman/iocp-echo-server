#include "net/receive_buffer.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace net {

void ReceiveBuffer::allocate(int capacity) {
    buf_   = std::make_unique<char[]>(static_cast<std::size_t>(capacity));
    cap_   = capacity;
    write_ = 0;
    read_  = 0;
}

void ReceiveBuffer::compact() noexcept {
    const int remain = write_ - read_;
    if (remain > 0 && read_ > 0) {
        // 구간이 겹칠 수 있으므로 memmove 를 쓴다
        std::memmove(buf_.get(), buf_.get() + read_, static_cast<std::size_t>(remain));
    }
    write_ = remain;
    read_  = 0;
}

bool ReceiveBuffer::grow(int add) {
    const int remain  = write_ - read_;
    const int new_cap = cap_ + add;
    if (new_cap > kMaxCapacity) return false;

    auto next = std::make_unique<char[]>(static_cast<std::size_t>(new_cap));
    if (remain > 0) {
        std::memcpy(next.get(), buf_.get() + read_, static_cast<std::size_t>(remain));
    }
    buf_   = std::move(next);
    cap_   = new_cap;
    write_ = remain;
    read_  = 0;
    return true;
}

bool ReceiveBuffer::reserve_free(int required) {
    if (required <= 0) required = kMinFree;

    if (!buf_) allocate(kChunk);

    // 전부 소비된 상태 - 확장분을 되돌리고 위치를 초기화한다.
    // 잔량이 있을 때 줄이면 이동이 한 번 더 생기므로 이 시점에만 축소한다
    if (read_ == write_) {
        if (cap_ > kChunk) {
            allocate(kChunk);
        } else {
            reset();
        }
    }

    if (writable_size() >= required) return true;

    // 1단 - 앞쪽 소비분을 회수하면 충분한 경우. 할당 없음
    if (writable_size() + read_ >= required) {
        compact();
        return true;
    }

    // 2단 - 상한 안에서 확장. 필요량을 덮을 때까지 chunk 단위로 늘린다
    int add = kChunk;
    while (writable_size() + read_ + add < required) add += kChunk;
    if (!grow(add)) return false;   // 3단 - 상한 초과

    return writable_size() >= required;
}

void ReceiveBuffer::commit_write(int bytes) noexcept {
    assert(bytes >= 0 && bytes <= writable_size());
    write_ += bytes;
}

void ReceiveBuffer::consume(int bytes) noexcept {
    assert(bytes >= 0 && bytes <= readable_size());
    read_ += bytes;
}

} // namespace net
