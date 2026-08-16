#include "app/handlers.h"

#include "app/reply.h"
#include "core/clock.h"
#include "core/log.h"
#include "proto/byte_reader.h"
#include "proto/byte_writer.h"

namespace app {
namespace {

void handle_echo(AppContext& ctx, Session& session, const InboundEvent& ev) {
    proto::ByteReader r(ev.body.data(), ev.size);
    const auto payload = r.str();

    if (!r.consumed_all()) {
        send_error(*session.conn, ev.msg_id, proto::kInvalidField, "echo body");
        ++ctx.rejected;
        return;
    }
    if (payload.size() > proto::kMaxPayloadBytes) {
        send_error(*session.conn, ev.msg_id, proto::kPayloadTooLong, "512 bytes max");
        ++ctx.rejected;
        return;
    }

    auto& w = scratch();
    w.str(payload);
    session.conn->send(proto::kEchoAck, w.data(), w.size());
}

void handle_ping(AppContext& ctx, Session& session, const InboundEvent& ev) {
    proto::ByteReader r(ev.body.data(), ev.size);
    const auto client_ms = r.u64();

    if (!r.consumed_all()) {
        send_error(*session.conn, ev.msg_id, proto::kInvalidField, "ping body");
        ++ctx.rejected;
        return;
    }

    // 받은 값을 그대로 되돌린다. 왕복 시간을 클라이언트가 직접 계산한다
    auto& w = scratch();
    w.u64(client_ms);
    w.u64(static_cast<std::uint64_t>(core::unix_ms()));
    session.conn->send(proto::kPongAck, w.data(), w.size());
}

// DB 요청을 큐에 넣는다. 여기서 기다리지 않는다
bool submit_db(AppContext& ctx, Session& session, std::uint16_t origin,
               std::unique_ptr<db::Request> req) {
    // 워커가 없는 구성이면 큐가 가득 찬 것이 아니라 DB 를 쓰지 않는 것이다.
    // 둘을 같은 코드로 답하면 원인을 잘못 알린다
    if (ctx.db == nullptr || !ctx.db->enabled()) {
        send_error(*session.conn, origin, proto::kDbUnavailable, "db disabled");
        ++ctx.rejected;
        return false;
    }

    req->conn_id      = session.conn_id;
    req->session_key  = session.session_key;
    req->submitted_us = core::now_us();

    // 기한은 큐에 넣는 시점부터 센다. 워커 앞에서 기다린 시간도 함께 들어간다
    if (ctx.db_deadline_us > 0) {
        req->deadline_us = req->submitted_us + ctx.db_deadline_us;
    }

    if (!ctx.db->submit(std::move(req))) {
        // 큐가 가득 찼다. 무한히 쌓느니 거절한다
        send_error(*session.conn, origin, proto::kServerBusy, "db queue full");
        ++ctx.rejected;
        return false;
    }

    ++ctx.db_requests;
    return true;
}

void handle_save(AppContext& ctx, Session& session, const InboundEvent& ev) {
    proto::ByteReader r(ev.body.data(), ev.size);
    const auto payload = r.str();

    if (!r.consumed_all()) {
        send_error(*session.conn, ev.msg_id, proto::kInvalidField, "save body");
        ++ctx.rejected;
        return;
    }
    if (payload.size() > proto::kMaxPayloadBytes) {
        send_error(*session.conn, ev.msg_id, proto::kPayloadTooLong, "512 bytes max");
        ++ctx.rejected;
        return;
    }

    auto req = std::make_unique<db::Request>();
    req->kind    = db::RequestKind::Save;
    req->payload.assign(payload);
    submit_db(ctx, session, ev.msg_id, std::move(req));
}

void handle_history(AppContext& ctx, Session& session, const InboundEvent& ev) {
    proto::ByteReader r(ev.body.data(), ev.size);
    const auto limit = r.u16();

    if (!r.consumed_all()) {
        send_error(*session.conn, ev.msg_id, proto::kInvalidField, "history body");
        ++ctx.rejected;
        return;
    }
    if (limit < proto::kMinHistoryLimit || limit > proto::kMaxHistoryLimit) {
        send_error(*session.conn, ev.msg_id, proto::kLimitOutOfRange, "1..100");
        ++ctx.rejected;
        return;
    }

    auto req = std::make_unique<db::Request>();
    req->kind  = db::RequestKind::History;
    req->limit = limit;
    submit_db(ctx, session, ev.msg_id, std::move(req));
}

void handle_counter(AppContext& ctx, Session& session, const InboundEvent& ev) {
    proto::ByteReader r(ev.body.data(), ev.size);
    const auto request_key = r.str();

    if (!r.consumed_all()) {
        send_error(*session.conn, ev.msg_id, proto::kInvalidField, "counter body");
        ++ctx.rejected;
        return;
    }
    // 멱등성 키는 36자 식별자다. 길이가 다르면 받지 않는다
    if (request_key.size() != 36) {
        send_error(*session.conn, ev.msg_id, proto::kInvalidField, "request_key must be 36 chars");
        ++ctx.rejected;
        return;
    }

    auto req = std::make_unique<db::Request>();
    req->kind = db::RequestKind::Counter;
    req->request_key.assign(request_key);
    submit_db(ctx, session, ev.msg_id, std::move(req));
}

void on_connected(AppContext& ctx, InboundEvent& ev) {
    Session* session = ctx.sessions->add(std::move(ev.conn));
    if (session == nullptr) return;

    auto& w = scratch();
    w.str(session->session_key);
    session->conn->send(proto::kSessionAck, w.data(), w.size());

    // 세션 키를 다른 서비스에 알린다. 표를 넘기지 않고 값만 넘긴다.
    // 받는 쪽은 이 값으로 자기 목록의 표시 이름을 채운다
    if (ctx.relay) {
        ServiceMessage msg;
        msg.kind        = ServiceEvent::SessionOpened;
        msg.conn_id     = session->conn_id;
        msg.text        = session->session_key;
        msg.enqueued_us = core::now_us();
        ctx.relay(std::move(msg));
    }

    LOG_DEBUG("session opened id=%llu key=%s",
              static_cast<unsigned long long>(session->conn_id),
              session->session_key.c_str());
}

void on_disconnected(AppContext& ctx, const InboundEvent& ev) {
    Session* session = ctx.sessions->find(ev.conn_id);
    if (session == nullptr) return;

    LOG_DEBUG("session closed id=%llu messages=%llu",
              static_cast<unsigned long long>(ev.conn_id),
              static_cast<unsigned long long>(session->message_count));

    // 세션이 붙잡고 있던 연결 참조도 여기서 놓인다.
    // 아직 DB 응답이 남아 있으면 그 응답은 세션을 찾지 못하고 버려진다
    ctx.sessions->remove(ev.conn_id);
}

void on_message(AppContext& ctx, InboundEvent& ev) {
    Session* session = ctx.sessions->find(ev.conn_id);
    if (session == nullptr) {
        // 종료가 먼저 처리된 뒤 도착한 항목이다. 버린다
        return;
    }

    ++session->message_count;
    ++ctx.handled;

    if (ev.oversized) {
        send_error(*session->conn, ev.msg_id, proto::kPayloadTooLong, "body too large");
        ++ctx.rejected;
        return;
    }

    switch (ev.msg_id) {
        case proto::kEchoReq:    handle_echo(ctx, *session, ev);    return;
        case proto::kPingReq:    handle_ping(ctx, *session, ev);    return;
        case proto::kSaveReq:    handle_save(ctx, *session, ev);    return;
        case proto::kHistoryReq: handle_history(ctx, *session, ev); return;
        case proto::kCounterReq: handle_counter(ctx, *session, ev); return;

        default:
            // 라우팅표가 이 서비스로 보낸 것만 여기 온다.
            // 표와 처리가 어긋났다는 뜻이므로 그대로 두지 않는다
            send_error(*session->conn, ev.msg_id, proto::kUnknownMessage, "unknown id");
            ++ctx.rejected;
            return;
    }
}

std::uint16_t request_msg_id(db::RequestKind kind) {
    switch (kind) {
        case db::RequestKind::Save:    return proto::kSaveReq;
        case db::RequestKind::History: return proto::kHistoryReq;
        case db::RequestKind::Counter: return proto::kCounterReq;
    }
    return 0;
}

} // namespace

void process_event(AppContext& ctx, InboundEvent& ev) {
    ctx.logic->assert_on_logic_thread("process_event");

    switch (ev.kind) {
        case EventKind::Connected:    on_connected(ctx, ev);    return;
        case EventKind::Disconnected: on_disconnected(ctx, ev); return;
        case EventKind::Message:      on_message(ctx, ev);      return;
    }
}

void process_db_response(AppContext& ctx, db::Response& res) {
    ctx.logic->assert_on_logic_thread("process_db_response");

    ++ctx.db_responses;
    ++ctx.db_responses_total;
    ctx.db_total_us += static_cast<std::uint64_t>(res.db_us);
    ctx.db_query_dist.add(res.db_us);

    // 질의 시간만 보면 워커 앞에서 기다린 구간이 빠진다.
    // 기다리는 쪽이 실제로 겪는 것은 이 값이다
    if (res.submitted_us > 0) {
        ctx.db_round_dist.add(core::now_us() - res.submitted_us);
    }

    Session* session = ctx.sessions->find(res.conn_id);
    if (session == nullptr) {
        // 응답이 오기 전에 끊겼다. 보낼 곳이 없다
        return;
    }

    const std::uint16_t origin = request_msg_id(res.kind);

    if (res.status != db::Status::Ok) {
        ++ctx.db_failures;

        const auto code = (res.status == db::Status::Unavailable)
                              ? proto::kDbUnavailable
                              : proto::kDbTimeout;

        const char* detail = "db request failed";
        if (res.status == db::Status::Expired) {
            ++ctx.db_expired;
            detail = "db request expired";
        }

        send_error(*session->conn, origin, code, detail);
        return;
    }

    auto& w = scratch();

    switch (res.kind) {
        case db::RequestKind::Save:
            w.u64(res.log_id);
            session->conn->send(proto::kSaveAck, w.data(), w.size());
            return;

        case db::RequestKind::History: {
            const auto count = static_cast<std::uint16_t>(res.rows.size());
            w.u16(count);
            for (const auto& row : res.rows) {
                w.u64(row.log_id);
                w.str(row.payload);
                w.u64(row.created_at_ms);
            }
            session->conn->send(proto::kHistoryAck, w.data(), w.size());
            return;
        }

        case db::RequestKind::Counter:
            w.u64(res.hit_count);
            // 중복 요청은 오류가 아니다. 같은 결과에 표시만 덧붙인다
            w.u8(res.already_applied ? 1u : 0u);
            session->conn->send(proto::kCounterAck, w.data(), w.size());
            return;
    }
}

} // namespace app
