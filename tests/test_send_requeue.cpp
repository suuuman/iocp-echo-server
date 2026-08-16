//
//  부분 전송 뒤 되돌리기 검증.
//
//  중첩 송신에서 부분 전송은 드물게 일어난다. 드물기 때문에 부하 시험으로는
//  잡히지 않고, 잡히지 않는 경로는 틀린 채로 남는다.
//  그래서 되돌리는 계산만 떼어 내 여기서 고정한다.
//
//  고정할 것은 두 가지다 - 보낸 만큼만 빠질 것, 그리고 남은 구간이
//  발행 중에 쌓인 것보다 **앞에** 설 것. 뒤에 서면 전송 순서가 뒤집힌다.
//
#include <string>
#include <vector>

#include "mini_check.h"
#include "net/connection.h"

namespace {

std::vector<char> bytes(const std::string& s) {
    return std::vector<char>(s.begin(), s.end());
}

std::string text(const std::vector<char>& v) {
    return std::string(v.begin(), v.end());
}

} // namespace

TEST_CASE("전량이 나갔으면 되돌릴 것이 없다") {
    std::vector<char> pending = bytes("CD");
    const std::vector<char> staged = bytes("AB");

    net::requeue_unsent(pending, staged, staged.size());
    CHECK_EQ(text(pending), std::string("CD"));
}

TEST_CASE("보낸 만큼만 빠지고 나머지가 되돌아온다") {
    std::vector<char> pending;
    const std::vector<char> staged = bytes("ABCDE");

    net::requeue_unsent(pending, staged, 2);
    CHECK_EQ(text(pending), std::string("CDE"));
}

TEST_CASE("남은 구간이 발행 중에 쌓인 것보다 앞에 선다") {
    // 발행 중에 "XY" 가 쌓였고, 보내던 "ABCD" 중 "AB" 만 나갔다.
    // 이어서 나가야 할 순서는 CD 다음 XY 다
    std::vector<char> pending = bytes("XY");
    const std::vector<char> staged = bytes("ABCD");

    net::requeue_unsent(pending, staged, 2);
    CHECK_EQ(text(pending), std::string("CDXY"));
}

TEST_CASE("한 바이트도 나가지 못하면 통째로 되돌아온다") {
    std::vector<char> pending = bytes("XY");
    const std::vector<char> staged = bytes("ABC");

    net::requeue_unsent(pending, staged, 0);
    CHECK_EQ(text(pending), std::string("ABCXY"));
}

TEST_CASE("여러 번 되돌려도 순서가 유지된다") {
    // 한 번에 다 나가지 않는 상황이 이어지는 경우다.
    // 되돌린 구간이 다시 앞에 서야 하므로 결과는 원래 순서 그대로여야 한다
    std::vector<char> pending;
    std::vector<char> staged = bytes("ABCDEF");

    net::requeue_unsent(pending, staged, 2);   // AB 나감 -> CDEF 남음
    staged = pending;
    pending.clear();

    net::requeue_unsent(pending, staged, 1);   // C 나감 -> DEF 남음
    CHECK_EQ(text(pending), std::string("DEF"));
}

TEST_CASE("빈 발행분은 아무것도 하지 않는다") {
    std::vector<char> pending = bytes("XY");
    const std::vector<char> staged;

    net::requeue_unsent(pending, staged, 0);
    CHECK_EQ(text(pending), std::string("XY"));
}
