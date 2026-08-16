#pragma once
//
//  접속자 표.
//
//  한 스레드만 만진다. 그래서 잠금이 없다.
//  "락이 없다" 는 것은 안전하다는 뜻이 아니라 한 스레드만 만진다는 약속이고,
//  약속은 검사로 지켜야 한다. 그래서 모든 진입부에 소속 검사를 건다.
//
//  잠금으로 지키는 방식도 함께 둔다(`session_table_guard`).
//  둘의 비용 차이는 측정에 있다(docs/benchmark.md).
//
//  다만 비용만 다른 것이 아니다 - 아래 두 함수는 표 안을 가리키는 포인터를 돌려준다.
//  소속 검사 방식에서는 그 포인터를 처리가 끝날 때까지 들고 있어도 된다.
//  같은 스레드만 표를 만지므로 그 사이에 원소가 사라지지 않기 때문이다.
//  여러 스레드가 함께 쓰는 표라면 잠금을 놓는 순간 그 보장이 없어지므로
//  값을 복사해 돌려주거나 잠금 구간을 처리 전체로 넓혀야 한다.
//  여기서 재는 것은 그 구조 차이가 아니라 **잠금 자체의 비용**이다.
//
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "core/logic_thread.h"
#include "net/connection.h"

namespace app {

// 표를 무엇으로 지키는가
enum class SessionGuard : std::uint8_t {
    // 소속 검사. 한 스레드만 만진다는 약속을 검사로 강제한다
    Owner,
    // 잠금. 진입부마다 잡는다
    Mutex,
};

struct Session {
    net::ConnectionId  conn_id      = 0;
    std::string        session_key;      // 서버가 발급한다
    net::ConnectionPtr conn;             // 세션이 사는 동안 연결을 붙잡는다
    std::int64_t       connected_ms = 0;
    std::uint64_t      message_count = 0;
};

class SessionTable {
public:
    explicit SessionTable(const core::LogicThread& logic,
                          SessionGuard guard = SessionGuard::Owner)
        : logic_(logic), guard_(guard) {}

    // 이미 있으면 기존 것을 돌려준다
    Session* add(net::ConnectionPtr conn);
    Session* find(net::ConnectionId id);
    bool     remove(net::ConnectionId id);

    std::size_t size() const;

    static SessionGuard parse_guard(const std::string& name);
    static const char*  guard_name(SessionGuard g);

private:
    // 진입부마다 만든다. 방식에 따라 검사이거나 잠금이다.
    //
    // 잠금을 직접 잡고 푸는 대신 이 객체에 맡긴다 -
    // 중간에서 예외가 빠져나가도 잠금이 풀린다.
    // 처리 중 예외는 항목 단위로 잡혀 다음 항목으로 넘어가므로,
    // 여기서 잠긴 채로 남으면 그 뒤의 모든 접근이 멈춘다
    class Access {
    public:
        Access(const SessionTable& table, const char* where);
        ~Access();

        Access(const Access&)            = delete;
        Access& operator=(const Access&) = delete;

    private:
        std::unique_lock<std::mutex> lock_;
    };

    const core::LogicThread&                       logic_;
    SessionGuard                                   guard_;
    mutable std::mutex                             mutex_;
    std::unordered_map<net::ConnectionId, Session> sessions_;
};

} // namespace app
