//
//  스트림 버퍼의 3단 여유 확보 정책 검증.
//
//  네트워크 버그의 상당수가 이 계층에서 생긴다.
//  부분 수신 · 병합 수신 · 재정렬 · 확장 · 축소를 각각 고정해 둔다.
//
#include <cstring>
#include <string>

#include "mini_check.h"
#include "net/receive_buffer.h"

using net::ReceiveBuffer;

namespace {

// 버퍼에 n 바이트를 밀어 넣는다
void write_bytes(ReceiveBuffer& b, char fill, int n) {
    CHECK(b.reserve_free(n));
    std::memset(b.writable(), fill, static_cast<std::size_t>(n));
    b.commit_write(n);
}

} // namespace

TEST_CASE("초기 상태는 비어 있다") {
    ReceiveBuffer b;
    CHECK(b.empty());
    CHECK_EQ(b.readable_size(), 0);
    CHECK_EQ(b.capacity(), 0);
}

TEST_CASE("첫 확보에서 기본 용량이 할당된다") {
    ReceiveBuffer b;
    CHECK(b.reserve_free());
    CHECK_EQ(b.capacity(), ReceiveBuffer::kChunk);
    CHECK(b.writable_size() >= ReceiveBuffer::kMinFree);
}

TEST_CASE("기록과 소비로 구간이 이동한다") {
    ReceiveBuffer b;
    write_bytes(b, 'a', 100);

    CHECK_EQ(b.readable_size(), 100);
    CHECK_EQ(b.readable()[0], 'a');

    b.consume(40);
    CHECK_EQ(b.readable_size(), 60);
    CHECK(!b.empty());

    b.consume(60);
    CHECK(b.empty());
}

TEST_CASE("1단 - 소비분 회수로 여유를 확보한다. 용량은 그대로다") {
    ReceiveBuffer b;

    // 여유가 최소치 아래로 떨어지도록 채운다
    const int fill = ReceiveBuffer::kChunk - ReceiveBuffer::kMinFree + 1024;
    write_bytes(b, 'a', fill);

    // 대부분을 소비해 앞쪽에 회수 가능한 구간을 만든다
    b.consume(fill - 16);
    CHECK_EQ(b.readable_size(), 16);

    const int cap_before = b.capacity();
    CHECK(b.reserve_free(ReceiveBuffer::kMinFree));

    // 재정렬만 일어나야 한다. 확장이 있었다면 용량이 늘었을 것이다
    CHECK_EQ(b.capacity(), cap_before);
    CHECK_EQ(b.readable_size(), 16);
    CHECK_EQ(b.readable()[0], 'a');
    CHECK(b.writable_size() >= ReceiveBuffer::kMinFree);
}

TEST_CASE("2단 - 회수해도 부족하면 확장한다. 데이터는 보존된다") {
    ReceiveBuffer b;

    // 소비하지 않고 가득 채운다. 회수할 앞 구간이 없다
    const int fill = ReceiveBuffer::kChunk - ReceiveBuffer::kMinFree + 1;
    write_bytes(b, 'z', fill);

    const int cap_before = b.capacity();
    CHECK(b.reserve_free(ReceiveBuffer::kMinFree));

    CHECK(b.capacity() > cap_before);
    CHECK_EQ(b.readable_size(), fill);
    CHECK_EQ(b.readable()[0], 'z');
    CHECK_EQ(b.readable()[fill - 1], 'z');
}

TEST_CASE("3단 - 상한을 넘으면 거부한다") {
    ReceiveBuffer b;
    CHECK(!b.reserve_free(ReceiveBuffer::kMaxCapacity + 1));
}

TEST_CASE("전부 소비되면 확장분을 되돌린다") {
    ReceiveBuffer b;

    // 한 번 확장시킨다
    write_bytes(b, 'q', ReceiveBuffer::kChunk - ReceiveBuffer::kMinFree + 1);
    CHECK(b.reserve_free(ReceiveBuffer::kMinFree));
    CHECK(b.capacity() > ReceiveBuffer::kChunk);

    // 전부 소비한 뒤 다시 확보하면 기본 용량으로 돌아와야 한다
    b.consume(b.readable_size());
    CHECK(b.empty());
    CHECK(b.reserve_free());
    CHECK_EQ(b.capacity(), ReceiveBuffer::kChunk);
}

TEST_CASE("잔량이 있으면 축소하지 않는다") {
    ReceiveBuffer b;

    write_bytes(b, 'w', ReceiveBuffer::kChunk - ReceiveBuffer::kMinFree + 1);
    CHECK(b.reserve_free(ReceiveBuffer::kMinFree));
    const int expanded = b.capacity();

    b.consume(b.readable_size() - 1);   // 1바이트 남긴다
    CHECK(b.reserve_free());

    // 줄이면 이동이 한 번 더 생긴다. 잔량이 있을 때는 유지한다
    CHECK_EQ(b.capacity(), expanded);
    CHECK_EQ(b.readable_size(), 1);
}
