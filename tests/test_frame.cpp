//
//  프레임 경계 복원과 필드 직렬화 검증.
//
//  TCP 는 경계를 보장하지 않는다.
//  한 프레임이 쪼개져 오는 경우와 여러 프레임이 붙어 오는 경우를 모두 고정한다.
//
#include <cstring>
#include <string>
#include <vector>

#include "mini_check.h"
#include "proto/byte_reader.h"
#include "proto/byte_writer.h"
#include "proto/frame.h"

using namespace proto;

namespace {

std::vector<char> make_frame(std::uint16_t id, std::string_view body) {
    std::vector<char> f(static_cast<std::size_t>(kHeaderSize) + body.size());
    write_header(f.data(), id, static_cast<std::uint32_t>(body.size()));
    std::memcpy(f.data() + kHeaderSize, body.data(), body.size());
    return f;
}

} // namespace

TEST_CASE("헤더보다 짧으면 미완성이다") {
    const char partial[3] = {0, 0, 0};
    FrameView v{};
    int consumed = 0;
    CHECK(peek_frame(partial, 3, v, consumed) == FrameResult::Incomplete);
}

TEST_CASE("본문이 덜 오면 미완성이다") {
    auto f = make_frame(7, "hello");
    FrameView v{};
    int consumed = 0;

    // 본문 1바이트를 뺀 상태
    CHECK(peek_frame(f.data(), static_cast<int>(f.size()) - 1, v, consumed)
          == FrameResult::Incomplete);

    // 전부 도착하면 잘라낸다
    CHECK(peek_frame(f.data(), static_cast<int>(f.size()), v, consumed)
          == FrameResult::Ok);
    CHECK_EQ(v.msg_id, 7);
    CHECK_EQ(v.size, 5u);
    CHECK_EQ(consumed, static_cast<int>(f.size()));
    CHECK(std::memcmp(v.body, "hello", 5) == 0);
}

TEST_CASE("본문 길이가 0인 프레임을 처리한다") {
    auto f = make_frame(5, "");
    FrameView v{};
    int consumed = 0;

    CHECK(peek_frame(f.data(), static_cast<int>(f.size()), v, consumed) == FrameResult::Ok);
    CHECK_EQ(v.size, 0u);
    CHECK_EQ(consumed, kHeaderSize);
}

TEST_CASE("붙어 온 프레임을 순서대로 잘라낸다") {
    auto a = make_frame(1, "first");
    auto b = make_frame(2, "second");

    std::vector<char> joined = a;
    joined.insert(joined.end(), b.begin(), b.end());

    int offset = 0;
    FrameView v{};
    int consumed = 0;

    CHECK(peek_frame(joined.data() + offset,
                     static_cast<int>(joined.size()) - offset, v, consumed)
          == FrameResult::Ok);
    CHECK_EQ(v.msg_id, 1);
    offset += consumed;

    CHECK(peek_frame(joined.data() + offset,
                     static_cast<int>(joined.size()) - offset, v, consumed)
          == FrameResult::Ok);
    CHECK_EQ(v.msg_id, 2);
    offset += consumed;

    CHECK_EQ(offset, static_cast<int>(joined.size()));
}

TEST_CASE("받는 본문 길이가 상한을 넘으면 거부한다") {
    char header[kHeaderSize]{};
    write_header(header, 1, kMaxInboundBody + 1);

    FrameView v{};
    int consumed = 0;
    CHECK(peek_frame(header, kHeaderSize, v, consumed) == FrameResult::TooLarge);
}

TEST_CASE("받는 상한과 같은 길이는 받아들인다") {
    // 경계값이 거부되면 정상 요청 하나가 통째로 막힌다
    std::vector<char> frame(kHeaderSize + kMaxInboundBody, 'x');
    write_header(frame.data(), 1, kMaxInboundBody);

    FrameView v{};
    int consumed = 0;
    CHECK(peek_frame(frame.data(), static_cast<int>(frame.size()), v, consumed)
          == FrameResult::Ok);
    CHECK_EQ(v.size, kMaxInboundBody);
    CHECK_EQ(consumed, static_cast<int>(frame.size()));
}

TEST_CASE("내보내는 상한이 받는 상한보다 크다") {
    // 요청은 작고 응답은 크다. 둘을 하나로 묶으면 받는 쪽 버퍼가 필요보다 커진다
    CHECK(kMaxOutboundBody > kMaxInboundBody);
}

TEST_CASE("정수 필드가 왕복한다") {
    ByteWriter w;
    w.u8(0xAB);
    w.u16(0x1234);
    w.u32(0xDEADBEEF);
    w.u64(0x0123456789ABCDEFull);

    ByteReader r(w.data(), w.size());
    CHECK_EQ(r.u8(), 0xAB);
    CHECK_EQ(r.u16(), 0x1234);
    CHECK_EQ(r.u32(), 0xDEADBEEFu);
    CHECK_EQ(r.u64(), 0x0123456789ABCDEFull);
    CHECK(r.consumed_all());
}

TEST_CASE("문자열이 왕복한다. 길이는 바이트 수다") {
    const std::string text = "한글 mixed";   // UTF-8

    ByteWriter w;
    w.str(text);

    ByteReader r(w.data(), w.size());
    const auto out = r.str();
    CHECK(r.consumed_all());
    CHECK_EQ(out.size(), text.size());
    CHECK(out == text);
}

TEST_CASE("범위를 벗어난 읽기는 ok 를 내린다") {
    ByteWriter w;
    w.u16(1);

    ByteReader r(w.data(), w.size());
    CHECK_EQ(r.u16(), 1);
    CHECK(r.ok());

    r.u32();              // 남은 바이트가 없다
    CHECK(!r.ok());
    CHECK(!r.consumed_all());
}

TEST_CASE("길이가 실제보다 큰 문자열은 거부된다") {
    // 길이 필드에 10을 적고 실제로는 2바이트만 넣는다
    char body[4]{};
    body[0] = 0; body[1] = 10;
    body[2] = 'a'; body[3] = 'b';

    ByteReader r(body, 4);
    const auto s = r.str();
    CHECK(s.empty());
    CHECK(!r.ok());
}
