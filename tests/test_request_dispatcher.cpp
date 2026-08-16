//
//  요청 배정 검증.
//
//  지켜야 하는 성질은 두 가지다.
//    ① 같은 키가 동시에 두 번 나가지 않는다 - 이것이 순서 보장의 근거다
//    ② 다른 키는 서로 막지 않는다 - 이것이 해시 고정과 갈리는 지점이다
//
#include <memory>
#include <string>
#include <vector>

#include "db/request_dispatcher.h"
#include "mini_check.h"

using db::Request;
using db::RequestDispatcher;

namespace {

constexpr std::size_t kBigCap = 1024;

std::unique_ptr<Request> make_request(const char* key, std::uint64_t tag) {
    auto req = std::make_unique<Request>();
    req->session_key = key;
    req->conn_id     = tag;   // 순서 확인용 표식
    return req;
}

} // namespace

TEST_CASE("빈 배정기는 내보낼 것이 없다") {
    RequestDispatcher d;
    CHECK(d.take() == nullptr);
    CHECK(!d.ready());
    CHECK_EQ(d.depth(), static_cast<std::size_t>(0));
}

TEST_CASE("같은 키는 앞선 처리가 끝날 때까지 다시 나오지 않는다") {
    RequestDispatcher d;
    CHECK(d.push(make_request("A", 1), kBigCap));
    CHECK(d.push(make_request("A", 2), kBigCap));

    auto first = d.take();
    CHECK(first != nullptr);
    CHECK_EQ(first->conn_id, static_cast<std::uint64_t>(1));

    // 두 번째 항목이 남아 있어도 내보내지 않는다
    CHECK(d.take() == nullptr);

    d.finish("A");
    auto second = d.take();
    CHECK(second != nullptr);
    CHECK_EQ(second->conn_id, static_cast<std::uint64_t>(2));   // 같은 키 안에서 순서 유지
}

TEST_CASE("다른 키는 서로 막지 않는다") {
    RequestDispatcher d;
    CHECK(d.push(make_request("A", 1), kBigCap));
    CHECK(d.push(make_request("B", 2), kBigCap));
    CHECK(d.push(make_request("C", 3), kBigCap));

    auto a = d.take();
    auto b = d.take();
    auto c = d.take();

    CHECK(a != nullptr);
    CHECK(b != nullptr);
    CHECK(c != nullptr);
    CHECK(d.take() == nullptr);

    // 세 키가 서로 다른 요청으로 나갔다
    CHECK(a->session_key != b->session_key);
    CHECK(b->session_key != c->session_key);
}

TEST_CASE("한 키가 밀려도 다른 키는 계속 나간다") {
    RequestDispatcher d;
    // A 에 다섯 건이 몰리고 B 는 한 건이다
    for (std::uint64_t i = 1; i <= 5; ++i) {
        CHECK(d.push(make_request("A", i), kBigCap));
    }
    CHECK(d.push(make_request("B", 100), kBigCap));

    auto first  = d.take();   // A
    auto second = d.take();   // B - A 가 밀려 있어도 나온다

    CHECK(first != nullptr);
    CHECK(second != nullptr);
    CHECK_EQ(first->session_key, std::string("A"));
    CHECK_EQ(second->session_key, std::string("B"));
}

TEST_CASE("처리 수는 응답 시점에 줄어든다") {
    RequestDispatcher d;
    CHECK(d.push(make_request("A", 1), kBigCap));
    CHECK(d.push(make_request("B", 2), kBigCap));
    CHECK_EQ(d.depth(), static_cast<std::size_t>(2));

    auto a = d.take();
    // 꺼냈다고 줄지 않는다. 아직 응답하지 않았다
    CHECK_EQ(d.depth(), static_cast<std::size_t>(2));

    d.finish("A");
    CHECK_EQ(d.depth(), static_cast<std::size_t>(1));
}

TEST_CASE("상한에 닿으면 받지 않는다") {
    RequestDispatcher d;
    CHECK(d.push(make_request("A", 1), 2));
    CHECK(d.push(make_request("B", 2), 2));
    CHECK(!d.push(make_request("C", 3), 2));

    CHECK_EQ(d.depth(), static_cast<std::size_t>(2));
}

TEST_CASE("처리가 끝나면 상한에 자리가 생긴다") {
    RequestDispatcher d;
    CHECK(d.push(make_request("A", 1), 1));
    CHECK(!d.push(make_request("B", 2), 1));

    auto a = d.take();
    d.finish("A");

    CHECK(d.push(make_request("B", 2), 1));
}

TEST_CASE("남은 대기분을 한 번에 회수한다") {
    RequestDispatcher d;
    CHECK(d.push(make_request("A", 1), kBigCap));
    CHECK(d.push(make_request("A", 2), kBigCap));
    CHECK(d.push(make_request("B", 3), kBigCap));

    auto working = d.take();   // A 하나는 처리 중이다
    CHECK(working != nullptr);

    // 처리 중인 것을 뺀 나머지가 회수된다
    auto rest = d.take_all();
    CHECK_EQ(rest.size(), static_cast<std::size_t>(2));
    CHECK(d.take() == nullptr);

    // 처리 중이던 키의 완료 통지는 여전히 받아야 한다
    d.finish("A");
    CHECK_EQ(d.depth(), static_cast<std::size_t>(0));
}

TEST_CASE("모르는 키를 끝냈다고 해도 무너지지 않는다") {
    RequestDispatcher d;
    d.finish("없는키");
    CHECK_EQ(d.depth(), static_cast<std::size_t>(0));

    CHECK(d.push(make_request("A", 1), kBigCap));
    CHECK_EQ(d.depth(), static_cast<std::size_t>(1));
}
