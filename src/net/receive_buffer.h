#pragma once
//
//  수신 바이트를 연속 메모리 한 덩어리에 누적하는 버퍼.
//
//  구간 구성
//      [0, read_)      이미 소비됨. 회수 가능
//      [read_, write_) 아직 처리되지 않은 데이터
//      [write_, cap_)  기록 가능한 여유
//
//  read_ 와 write_ 는 전진만 하고 되돌아가지 않는다.
//  소비된 앞쪽 구간은 즉시 회수하지 않고 여유가 부족해진 시점에 한 번에 정리한다.
//
#include <cstddef>
#include <memory>

#include "proto/frame.h"

namespace net {

class ReceiveBuffer {
public:
    // 용량은 이 서버가 받는 프레임 크기에서 유도한다.
    //   일반 요청     = 헤더 6 + 길이 2 + 본문 512      = 520바이트
    //   받는 최대 프레임 = 헤더 6 + 수신 본문 상한 1KB  = 1,030바이트

    // 한 번 읽기에 일반 요청 7건이 들어가는 크기
    static constexpr int kMinFree = 4 * 1024;
    // 초기 용량이자 1회 확장량이며 축소 기준이기도 하다.
    // 일반 요청 15건 분량
    static constexpr int kChunk   = 8 * 1024;
    // 연결당 상한. 넘으면 수신을 거부하고 연결을 끊는다.
    //
    // 이 값이 곧 접속당 메모리이고, 접속 상한과 곱해져 서버 전체의 최악값이 된다.
    // 받는 프레임 상한(1KB)에 맞춰 두어야 그 계산이 실제와 맞는다
    static constexpr int kMaxCapacity = 16 * 1024;

    ReceiveBuffer() = default;
    ~ReceiveBuffer() = default;

    ReceiveBuffer(const ReceiveBuffer&)            = delete;
    ReceiveBuffer& operator=(const ReceiveBuffer&) = delete;
    ReceiveBuffer(ReceiveBuffer&&) noexcept            = default;
    ReceiveBuffer& operator=(ReceiveBuffer&&) noexcept = default;

    // 수신 요청에 넘길 여유 구간을 확보한다.
    // 3단 정책 : 재정렬 → 확장 → 거부
    // 반환 false 는 상한 초과다. 호출부는 연결을 끊어야 한다
    bool reserve_free(int required = kMinFree);

    // 수신 완료 통보로 받은 바이트 수를 반영한다
    void commit_write(int bytes) noexcept;
    // 처리한 바이트 수만큼 소비 위치를 전진시킨다
    void consume(int bytes) noexcept;

    char*       readable()       noexcept { return buf_.get() + read_; }
    const char* readable() const noexcept { return buf_.get() + read_; }
    int         readable_size() const noexcept { return write_ - read_; }

    char* writable()       noexcept { return buf_.get() + write_; }
    int   writable_size() const noexcept { return cap_ - write_; }

    int  capacity() const noexcept { return cap_; }
    bool empty()    const noexcept { return read_ == write_; }

    // 소비 완료 상태에서만 호출한다. 위치를 0으로 되돌린다
    void reset() noexcept { read_ = write_ = 0; }

private:
    void allocate(int capacity);
    void compact() noexcept;   // 남은 데이터를 앞으로 이동
    bool grow(int add);        // 확장 후 남은 데이터 이전

    std::unique_ptr<char[]> buf_;
    int cap_   = 0;
    int write_ = 0;   // 기록 위치
    int read_  = 0;   // 소비 위치
};

} // namespace net
