#include "app/service_router.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "app/reply.h"
#include "core/clock.h"
#include "core/log.h"

namespace app {

bool ServiceRouter::start(const Options& opt, db::WorkerPool* db) {
    table_.fill(kNoService);

    const auto assign = [this](std::uint16_t msg_id, ServiceId id) {
        table_[msg_id] = static_cast<std::uint8_t>(id);
    };

    assign(proto::kEchoReq,    ServiceId::Game);
    assign(proto::kSaveReq,    ServiceId::Game);
    assign(proto::kHistoryReq, ServiceId::Game);
    assign(proto::kCounterReq, ServiceId::Game);
    assign(proto::kPingReq,    ServiceId::Game);
    assign(proto::kChatReq,    ServiceId::Chat);

    // 세션 키가 나오면 채팅 서비스로 넘긴다.
    // 게임 서비스는 상대가 누구인지 모른 채 라우터에 맡기기만 한다
    game_.configure(db, opt.db_request_timeout_ms, opt.session_guard,
                    [this](ServiceMessage msg) { post_to(ServiceId::Chat, std::move(msg)); });

    Service::Options game_opt{};
    game_opt.lane_count        = opt.game_lane_count;
    game_opt.stats_interval_ms = opt.stats_interval_ms;
    game_opt.inbound_queue_cap = opt.inbound_queue_cap;
    game_opt.overflow_policy   = opt.overflow_policy;

    if (!game_.start(game_opt)) return false;

    // 채팅은 접속자 전원이 한 스레드에 모여야 브로드캐스트가 그 안에서 끝난다
    Service::Options chat_opt = game_opt;
    chat_opt.lane_count       = 1;

    if (!chat_.start(chat_opt)) {
        game_.stop();
        return false;
    }

    LOG_INFO("service router started game_lanes=%d chat_lanes=%d session_guard=%s",
             game_.lane_count(), chat_.lane_count(),
             SessionTable::guard_name(opt.session_guard));
    return true;
}

void ServiceRouter::stop() {
    chat_.stop();
    game_.stop();
}

Service* ServiceRouter::service_at(std::size_t index) noexcept {
    switch (static_cast<ServiceId>(index)) {
        case ServiceId::Game: return &game_;
        case ServiceId::Chat: return &chat_;
        default:              return nullptr;
    }
}

Service* ServiceRouter::route(std::uint16_t msg_id) noexcept {
    if (msg_id >= kRouteTableSize) return nullptr;

    const std::uint8_t slot = table_[msg_id];
    if (slot == kNoService) return nullptr;

    return service_at(slot);
}

void ServiceRouter::post_connected(const net::ConnectionPtr& conn) {
    for (std::size_t i = 0; i < kServiceCount; ++i) {
        service_at(i)->post_connected(conn);
    }
}

void ServiceRouter::post_disconnected(net::ConnectionId id) {
    for (std::size_t i = 0; i < kServiceCount; ++i) {
        service_at(i)->post_disconnected(id);
    }
}

bool ServiceRouter::post_message(net::Connection& conn, const proto::FrameView& frame) {
    Service* target = route(frame.msg_id);

    if (target == nullptr) {
        // 어느 서비스도 맡지 않는다. 큐에 넣을 곳이 없으므로 여기서 답한다.
        // 세션을 찾을 필요가 없다 - 답할 상대는 이 연결 자신이다
        unknown_.fetch_add(1, std::memory_order_relaxed);
        send_error(conn, frame.msg_id, proto::kUnknownMessage, "unknown id");
        return true;
    }
    return target->post_message(conn, frame);
}

void ServiceRouter::post_db_response(std::unique_ptr<db::Response> res) {
    game_.post_db_response(std::move(res));
}

void ServiceRouter::post_to(ServiceId target, ServiceMessage msg) {
    Service* service = service_at(static_cast<std::size_t>(target));
    if (service == nullptr) return;

    service->post_service(std::move(msg));
}

void ServiceRouter::drain(int timeout_ms) {
    const std::int64_t deadline = core::now_ms() + std::max(0, timeout_ms);

    for (;;) {
        std::size_t left = 0;
        for (std::size_t i = 0; i < kServiceCount; ++i) {
            left += service_at(i)->pending();
        }
        if (left == 0) break;

        if (core::now_ms() >= deadline) {
            LOG_WARN("service drain timeout - %zu events remain", left);
            return;
        }

        // 소비가 신호로 도므로 깨워 두지 않으면 유휴 상한만큼 늦어진다
        for (std::size_t i = 0; i < kServiceCount; ++i) {
            service_at(i)->wake_all();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    LOG_INFO("service drain done");
}

ServiceStats ServiceRouter::stats() const {
    // **개수만** 합친다. 분포는 합치지 않는다.
    //
    // 두 서비스가 재는 "항목 하나" 가 서로 다르기 때문이다 -
    // 게임의 한 건은 메시지 하나이고, 채팅의 한 건은 접속자 전원에게 보내는 일이다.
    // 그 둘을 한 분포로 묶으면 어느 쪽도 설명하지 못하는 값이 나온다.
    // 분포는 game_stats() · chat_stats() 로 서비스마다 따로 읽는다
    ServiceStats total{};
    const ServiceStats g = game_.stats();
    const ServiceStats c = chat_.stats();

    for (const ServiceStats* s : { &g, &c }) {
        total.entries        += s->entries;
        total.handled        += s->handled;
        total.rejected       += s->rejected;
        total.faults         += s->faults;
        total.overflow       += s->overflow;
        total.relay_messages += s->relay_messages;

        total.db_requests  += s->db_requests;
        total.db_responses += s->db_responses;
        total.db_failures  += s->db_failures;
        total.db_expired   += s->db_expired;

        total.broadcasts      += s->broadcasts;
        total.broadcast_sends += s->broadcast_sends;
    }

    // 라우팅에서 되돌린 것도 오류로 답한 메시지다
    total.rejected += unknown_.load(std::memory_order_relaxed);

    return total;
}

} // namespace app
