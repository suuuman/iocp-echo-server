#include "app/game_service.h"

#include <exception>

#include "core/clock.h"
#include "core/log.h"

namespace app {

void GameService::configure(db::WorkerPool* db, int db_request_timeout_ms,
                            SessionGuard guard, RelayFn relay) {
    db_          = db;
    deadline_us_ = db_request_timeout_ms > 0
                       ? static_cast<std::int64_t>(db_request_timeout_ms) * 1000
                       : 0;
    guard_       = guard;
    relay_       = std::move(relay);
}

void GameService::on_stopped() {
    // 세션 표가 레인의 로직 스레드를 참조한다. 레인이 사라지기 전에 놓는다
    states_.clear();
}

bool GameService::on_created(int count) {
    states_.clear();
    states_.reserve(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i) {
        auto state = std::make_unique<State>();
        Lane& lane = lane_at(i);

        state->db_inbox.reserve(1024);

        // 세션 표는 자기 레인의 스레드만 만진다. 소속 검사가 그것을 강제한다.
        // 잠금으로 지키는 방식도 설정으로 고를 수 있다 - 비용 비교용이다
        state->sessions = std::make_unique<SessionTable>(lane.logic, guard_);

        state->ctx.logic          = &lane.logic;
        state->ctx.sessions       = state->sessions.get();
        state->ctx.db             = db_;
        state->ctx.db_deadline_us = deadline_us_;
        state->ctx.relay          = relay_;

        states_.push_back(std::move(state));
    }
    return true;
}

void GameService::on_before_batch(Lane& lane) {
    State& state = state_of(lane);

    // DB 응답을 먼저 비운다. 기다리는 쪽을 우선한다
    auto& replies = state.db_inbox.swap();
    for (auto& res : replies) {
        if (!res) continue;
        try {
            process_db_response(state.ctx, *res);
        } catch (const std::exception& e) {
            note_fault(lane, res->conn_id, e.what());
        } catch (...) {
            note_fault(lane, res->conn_id, "?");
        }
    }
    replies.clear();
}

void GameService::on_event(Lane& lane, InboundEvent& ev) {
    process_event(state_of(lane).ctx, ev);
}

void GameService::on_service(Lane& lane, ServiceMessage& msg) {
    // 지금은 게임 서비스가 받을 것이 없다. 채팅 서비스가 상태를 되돌려 줄 일이 생기면
    // 여기에 붙는다 - 골격이 큐와 통계를 이미 갖고 있으므로 처리만 더하면 된다
    (void)lane;
    (void)msg;
}

void GameService::on_fault(Lane& lane, net::ConnectionId id) {
    AppContext& ctx = state_of(lane).ctx;

    Session* session = ctx.sessions->find(id);
    if (session == nullptr) return;

    if (session->conn) session->conn->close();
    ctx.sessions->remove(id);
}

std::size_t GameService::on_pending(Lane& lane) {
    return state_of(lane).db_inbox.pending();
}

void GameService::on_report(Lane& lane, ServiceStats& out) {
    AppContext& ctx = state_of(lane).ctx;
    const auto dbn  = ctx.db_responses ? ctx.db_responses : 1;

    LOG_INFO("game lane=%d sessions=%zu handled=%llu rejected=%llu | "
             "db req=%llu res=%llu fail=%llu exp=%llu avg=%.0fus",
             lane.index,
             ctx.sessions->size(),
             static_cast<unsigned long long>(ctx.handled),
             static_cast<unsigned long long>(ctx.rejected),
             static_cast<unsigned long long>(ctx.db_requests),
             static_cast<unsigned long long>(ctx.db_responses),
             static_cast<unsigned long long>(ctx.db_failures),
             static_cast<unsigned long long>(ctx.db_expired),
             static_cast<double>(ctx.db_total_us) / static_cast<double>(dbn));

    // 질의 시간과 왕복 시간을 나란히 둔다. 차이가 워커 앞에서 기다린 구간이다
    if (ctx.db_round_dist.count() > 0) {
        LOG_INFO("game lane=%d db query p50/p95/p99=%lld/%lld/%lldus | "
                 "round p50/p95/p99=%lld/%lld/%lldus max=%lldus n=%llu",
                 lane.index,
                 static_cast<long long>(ctx.db_query_dist.percentile(0.50)),
                 static_cast<long long>(ctx.db_query_dist.percentile(0.95)),
                 static_cast<long long>(ctx.db_query_dist.percentile(0.99)),
                 static_cast<long long>(ctx.db_round_dist.percentile(0.50)),
                 static_cast<long long>(ctx.db_round_dist.percentile(0.95)),
                 static_cast<long long>(ctx.db_round_dist.percentile(0.99)),
                 static_cast<long long>(ctx.db_round_dist.max()),
                 static_cast<unsigned long long>(ctx.db_round_dist.count()));
    }

    out.entries  = ctx.sessions->size();
    out.handled  = ctx.handled;
    out.rejected = ctx.rejected;

    out.db_requests   = ctx.db_requests;
    out.db_responses  = ctx.db_responses_total;
    out.db_failures   = ctx.db_failures;
    out.db_expired    = ctx.db_expired;
    out.db_round_dist = ctx.db_round_dist;

    ctx.db_total_us  = 0;
    ctx.db_responses = 0;

    ctx.db_query_dist.reset();
    ctx.db_round_dist.reset();
}

void GameService::post_db_response(std::unique_ptr<db::Response> res) {
    // 서비스가 이미 정리된 뒤에도 워커 종료분이 들어올 수 있다
    if (!res || lane_count() == 0) return;

    // 요청을 낸 레인으로 돌려보낸다. 다른 레인은 그 세션을 알지 못한다
    Lane&  lane  = lane_of(res->conn_id);
    State& state = state_of(lane);

    state.db_inbox.push(std::move(res));
    lane.logic.wake();
}

} // namespace app
