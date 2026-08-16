#pragma once
//
//  게임 서비스.
//
//  세션 표와 DB 요청을 소유한다. 담당 메시지는 Echo · Save · History · Counter · Ping 이다.
//
//  접속하면 여기서 세션 키를 발급하고, 그 사실을 채팅 서비스에 넘긴다.
//  넘기는 것은 값뿐이다 - 채팅 서비스가 이 표를 들여다보지 않는다.
//
//  DB 응답은 요청을 낸 레인으로 되돌린다. 다른 레인은 그 세션을 알지 못한다.
//
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "app/handlers.h"
#include "app/service.h"
#include "app/session_table.h"
#include "core/swap_queue.h"
#include "db/db_types.h"
#include "db/db_worker_pool.h"

namespace app {

class GameService : public Service {
public:
    // 세션 키가 나왔을 때 부른다. 라우터가 채팅 서비스로 넘긴다
    using RelayFn = std::function<void(ServiceMessage)>;

    GameService() : Service("game") {}
    // 세션 표가 레인의 로직 스레드를 참조한다. 그 스레드보다 먼저 사라지면 안 된다
    ~GameService() override { stop(); }

    void configure(db::WorkerPool* db, int db_request_timeout_ms,
                   SessionGuard guard, RelayFn relay);

    // DB 워커에서 호출한다
    void post_db_response(std::unique_ptr<db::Response> res);

protected:
    bool on_created(int count) override;
    void on_event(Lane& lane, InboundEvent& ev) override;
    void on_service(Lane& lane, ServiceMessage& msg) override;
    void on_fault(Lane& lane, net::ConnectionId id) override;
    void on_report(Lane& lane, ServiceStats& out) override;
    std::size_t on_pending(Lane& lane) override;
    void on_before_batch(Lane& lane) override;
    void on_stopped() override;

private:
    // 레인마다 하나씩. 자기 레인의 스레드만 만진다
    struct State {
        std::unique_ptr<SessionTable>                  sessions;
        core::SwapQueue<std::unique_ptr<db::Response>> db_inbox;
        AppContext                                     ctx;
    };

    State& state_of(const Lane& lane) noexcept {
        return *states_[static_cast<std::size_t>(lane.index)];
    }

    std::vector<std::unique_ptr<State>> states_;
    db::WorkerPool*                     db_          = nullptr;
    std::int64_t                        deadline_us_ = 0;
    SessionGuard                        guard_       = SessionGuard::Owner;
    RelayFn                             relay_;
};

} // namespace app
