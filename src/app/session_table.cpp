#include "app/session_table.h"

#include "core/clock.h"
#include "core/uuid.h"

namespace app {

SessionGuard SessionTable::parse_guard(const std::string& name) {
    return name == "mutex" ? SessionGuard::Mutex : SessionGuard::Owner;
}

const char* SessionTable::guard_name(SessionGuard g) {
    return g == SessionGuard::Mutex ? "mutex" : "owner";
}

SessionTable::Access::Access(const SessionTable& table, const char* where)
    : lock_(table.mutex_, std::defer_lock) {
    if (table.guard_ == SessionGuard::Mutex) {
        lock_.lock();
        return;
    }
    // 잠그지 않는다. 대신 한 스레드만 만진다는 약속을 검사로 강제한다
    table.logic_.assert_on_logic_thread(where);
}

SessionTable::Access::~Access() = default;

Session* SessionTable::add(net::ConnectionPtr conn) {
    const Access guard(*this, "SessionTable::add");
    if (!conn) return nullptr;

    const net::ConnectionId id = conn->id();

    const auto it = sessions_.find(id);
    if (it != sessions_.end()) return &it->second;

    Session s;
    s.conn_id      = id;
    // 세션 키는 서버가 만든다. 클라이언트가 보낸 값을 쓰면
    // 다른 세션의 기록을 조회할 수 있게 된다
    s.session_key  = core::make_session_key();
    s.conn         = std::move(conn);
    s.connected_ms = core::now_ms();

    return &sessions_.emplace(id, std::move(s)).first->second;
}

Session* SessionTable::find(net::ConnectionId id) {
    const Access guard(*this, "SessionTable::find");

    const auto it = sessions_.find(id);
    return (it == sessions_.end()) ? nullptr : &it->second;
}

bool SessionTable::remove(net::ConnectionId id) {
    const Access guard(*this, "SessionTable::remove");
    return sessions_.erase(id) > 0;
}

std::size_t SessionTable::size() const {
    const Access guard(*this, "SessionTable::size");
    return sessions_.size();
}

} // namespace app
