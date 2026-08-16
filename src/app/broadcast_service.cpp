#include "app/broadcast_service.h"

#include "app/reply.h"
#include "core/clock.h"
#include "core/log.h"
#include "proto/byte_reader.h"
#include "proto/byte_writer.h"

namespace app {

void BroadcastService::on_stopped() {
    // 목록이 붙잡고 있던 연결 참조를 놓는다
    members_.clear();
    pending_names_.clear();
}

bool BroadcastService::on_created(int count) {
    members_.clear();
    pending_names_.clear();

    if (count != 1) {
        // 목록이 레인마다 갈라지면 브로드캐스트가 레인 간 전달이 된다
        LOG_ERROR("chat service requires exactly one lane (got %d)", count);
        return false;
    }
    members_.reserve(1024);
    return true;
}

void BroadcastService::on_event(Lane& lane, InboundEvent& ev) {
    (void)lane;

    switch (ev.kind) {
        case EventKind::Connected: {
            if (!ev.conn) return;

            Member& m = members_[ev.conn_id];
            m.conn    = std::move(ev.conn);

            // 세션 키는 게임 서비스가 발급한다.
            // 이 통지보다 먼저 도착했으면 맡겨 둔 자리에서 가져온다
            const auto pending = pending_names_.find(ev.conn_id);
            if (pending != pending_names_.end()) {
                m.name = std::move(pending->second);
                pending_names_.erase(pending);
            }
            return;
        }

        case EventKind::Disconnected:
            members_.erase(ev.conn_id);
            pending_names_.erase(ev.conn_id);
            return;

        case EventKind::Message:
            break;
    }

    const auto it = members_.find(ev.conn_id);
    if (it == members_.end()) {
        // 종료가 먼저 처리된 뒤 도착한 항목이다. 버린다
        return;
    }
    Member& member = it->second;

    ++handled;

    if (ev.oversized) {
        send_error(*member.conn, ev.msg_id, proto::kPayloadTooLong, "body too large");
        ++rejected;
        return;
    }

    proto::ByteReader r(ev.body.data(), ev.size);
    const auto text = r.str();

    if (!r.consumed_all()) {
        send_error(*member.conn, ev.msg_id, proto::kInvalidField, "chat body");
        ++rejected;
        return;
    }
    if (text.size() > proto::kMaxPayloadBytes) {
        send_error(*member.conn, ev.msg_id, proto::kPayloadTooLong, "512 bytes max");
        ++rejected;
        return;
    }

    // 보낸 사람이 아직 세션 키를 받지 못했다면 그 사실을 이름 자리에 그대로 둔다.
    // 값을 지어내면 받는 쪽이 실제 세션과 구분할 수 없다
    broadcast(member.name, text);
}

void BroadcastService::on_service(Lane& lane, ServiceMessage& msg) {
    (void)lane;

    if (msg.kind != ServiceEvent::SessionOpened) return;

    const auto it = members_.find(msg.conn_id);
    if (it == members_.end()) {
        // 접속 통지를 아직 처리하지 않았다. 그 통지가 이 회차의 수신 큐에 있으므로
        // 여기 맡겨 두면 곧바로 옮겨 간다. 그러지 않고 버리면 표시 이름이 비어 버린다
        pending_names_[msg.conn_id] = std::move(msg.text);
        return;
    }
    it->second.name = std::move(msg.text);
}

void BroadcastService::on_after_batch(Lane& lane) {
    (void)lane;

    // 이 회차의 수신 큐를 다 비웠다. 여기까지 남은 세션 키는 주인이 없다 -
    // 접속 통지는 세션 키보다 먼저 적재되므로, 쓸모 있는 것이면 방금 옮겨졌다.
    // 남겨 두면 접속·종료를 되풀이할수록 이 표만 자란다
    if (pending_names_.empty()) return;

    orphan_names += pending_names_.size();
    pending_names_.clear();
}

void BroadcastService::on_fault(Lane& lane, net::ConnectionId id) {
    (void)lane;

    pending_names_.erase(id);

    const auto it = members_.find(id);
    if (it == members_.end()) return;

    if (it->second.conn) it->second.conn->close();
    members_.erase(it);
}

void BroadcastService::broadcast(const std::string& from, std::string_view text) {
    const std::int64_t begin_us = core::now_us();

    auto& w = scratch();
    w.str(from);
    w.str(text);

    // 본문을 한 번만 만들어 모든 연결에 같은 바이트를 넘긴다.
    // 접속이 많으면 이 한 번이 접속 수만큼의 송신이다 -
    // 그 사이 느린 연결은 송신 누적 상한에 걸려 끊길 수 있고, 끊긴 연결의
    // 종료 통지는 큐로 들어오므로 이 반복문이 표를 건드리지는 않는다
    std::uint64_t sends = 0;
    for (auto& entry : members_) {
        net::Connection* conn = entry.second.conn.get();
        if (conn == nullptr || conn->closed()) continue;

        conn->send(proto::kChatPush, w.data(), w.size());
        ++sends;
    }

    ++broadcasts;
    broadcast_sends += sends;
    broadcast_dist.add(core::now_us() - begin_us);
}

void BroadcastService::on_report(Lane& lane, ServiceStats& out) {
    LOG_INFO("chat lane=%d members=%zu pending=%zu orphan=%llu handled=%llu rejected=%llu | "
             "broadcasts=%llu sends=%llu fanout=%.1f",
             lane.index,
             members_.size(),
             pending_names_.size(),
             static_cast<unsigned long long>(orphan_names),
             static_cast<unsigned long long>(handled),
             static_cast<unsigned long long>(rejected),
             static_cast<unsigned long long>(broadcasts),
             static_cast<unsigned long long>(broadcast_sends),
             broadcasts ? static_cast<double>(broadcast_sends) /
                              static_cast<double>(broadcasts)
                        : 0.0);

    // 브로드캐스트 하나에 든 시간은 접속 수에 비례해 자란다.
    // 평균만 보면 그 꼬리가 보이지 않는다
    if (broadcast_dist.count() > 0) {
        LOG_INFO("chat lane=%d broadcast p50/p95/p99=%lld/%lld/%lldus max=%lldus n=%llu",
                 lane.index,
                 static_cast<long long>(broadcast_dist.percentile(0.50)),
                 static_cast<long long>(broadcast_dist.percentile(0.95)),
                 static_cast<long long>(broadcast_dist.percentile(0.99)),
                 static_cast<long long>(broadcast_dist.max()),
                 static_cast<unsigned long long>(broadcast_dist.count()));
    }

    out.entries  = members_.size();
    out.handled  = handled;
    out.rejected = rejected;

    out.broadcasts      = broadcasts;
    out.broadcast_sends = broadcast_sends;
    out.broadcast_dist  = broadcast_dist;

    broadcast_dist.reset();
}

} // namespace app
