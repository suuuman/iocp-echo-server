#include "app/service.h"

#include <algorithm>
#include <cstring>
#include <exception>

#include "app/reply.h"
#include "core/clock.h"
#include "core/log.h"

namespace app {

Service::~Service() {
    stop();
}

bool Service::start(const Options& opt) {
    if (!lanes_.empty()) return true;

    const int count = std::clamp(opt.lane_count <= 0 ? 1 : opt.lane_count, 1, 64);

    // 하한을 낮게 둔다. 작은 값으로 두어야 가득 찬 상황을 실제로 만들어
    // 정책(거절 · 역압)이 어떻게 다른지 잴 수 있다.
    // 운영값은 설정으로 크게 잡는다
    inbound_cap_ = static_cast<std::size_t>(std::max(64, opt.inbound_queue_cap));
    policy_      = opt.overflow_policy;

    lanes_.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        lanes_.push_back(std::make_unique<Lane>());
        lanes_.back()->index = i;
    }

    for (auto& holder : lanes_) {
        holder->inbound.reserve(std::min<std::size_t>(4096, inbound_cap_));
        holder->service_inbox.reserve(256);
    }

    // 상태를 먼저 만든다. 스레드가 돌기 시작하면 그 상태를 곧바로 만진다
    if (!on_created(count)) {
        lanes_.clear();
        return false;
    }

    for (auto& holder : lanes_) {
        Lane& lane = *holder;

        if (opt.stats_interval_ms > 0) {
            lane.logic.add_timer(opt.stats_interval_ms, [this, &lane] { report(lane); });
        }
        if (!lane.logic.start([this, &lane] { run_step(lane); })) {
            LOG_ERROR("%s service lane %d start failed", name_, lane.index);
            stop();
            return false;
        }
    }

    LOG_INFO("%s service started lanes=%d queue_cap=%zu on_full=%s",
             name_, count, inbound_cap_, policy_name(policy_));
    return true;
}

void Service::stop() {
    // 스레드를 먼저 멈춘다. 그 뒤에야 파생 상태를 놓을 수 있다 -
    // 도는 중에 놓으면 그 스레드가 사라진 상태를 만진다
    for (auto& holder : lanes_) holder->logic.stop();
    on_stopped();
    lanes_.clear();
}

std::size_t Service::pending() {
    std::size_t left = 0;
    for (auto& holder : lanes_) {
        Lane& lane = *holder;
        left += lane.inbound.pending();
        left += lane.service_inbox.pending();
        left += on_pending(lane);
    }
    return left;
}

void Service::wake_all() {
    for (auto& holder : lanes_) holder->logic.wake();
}

OverflowPolicy Service::parse_policy(const std::string& name) {
    return name == "backpressure" ? OverflowPolicy::Backpressure : OverflowPolicy::Reject;
}

const char* Service::policy_name(OverflowPolicy p) {
    return p == OverflowPolicy::Backpressure ? "backpressure" : "reject";
}

ServiceStats Service::stats() const {
    ServiceStats total{};

    for (const auto& holder : lanes_) {
        std::lock_guard<std::mutex> guard(holder->stats_mutex);
        const ServiceStats& s = holder->stats;

        total.entries        += s.entries;
        total.handled        += s.handled;
        total.rejected       += s.rejected;
        total.faults         += s.faults;
        total.overflow       += s.overflow;
        total.relay_messages += s.relay_messages;

        total.db_requests    += s.db_requests;
        total.db_responses   += s.db_responses;
        total.db_failures    += s.db_failures;
        total.db_expired     += s.db_expired;

        total.broadcasts      += s.broadcasts;
        total.broadcast_sends += s.broadcast_sends;

        // 레인은 같은 종류의 일을 나눠 맡는다. 그래서 분포를 합칠 수 있다.
        // 합친 뒤 백분위를 내면 "가장 나쁜 레인" 을 쓸 때 생기던 비관이 사라진다
        total.process_dist.merge(s.process_dist);
        total.queue_dist.merge(s.queue_dist);
        total.relay_dist.merge(s.relay_dist);
        total.db_round_dist.merge(s.db_round_dist);
        total.broadcast_dist.merge(s.broadcast_dist);
    }
    return total;
}

void Service::note_fault(Lane& lane, net::ConnectionId id, const char* what) {
    ++lane.faults;
    LOG_ERROR("%s handler fault id=%llu: %s",
              name_, static_cast<unsigned long long>(id), what ? what : "?");

    // 처리가 중간에 끊겼으므로 이 세션의 상태가 어디까지 반영됐는지 알 수 없다.
    // 남겨 두면 다음 메시지가 같은 자리에서 다시 튕긴다
    on_fault(lane, id);
}

void Service::run_step(Lane& lane) {
    // 파생이 먼저 비울 것이 있으면 여기서 비운다(게임 서비스의 DB 응답)
    on_before_batch(lane);

    // 서비스 간 전달을 수신 큐보다 먼저 비운다.
    // 큐가 둘로 나뉘어 있으므로 순서를 정해 두지 않으면 같은 접속에 대해
    // "세션 정보보다 그 세션의 메시지가 먼저" 처리되는 역전이 생긴다
    auto& relays = lane.service_inbox.swap();
    for (auto& msg : relays) {
        lane.relay_dist.add(core::now_us() - msg.enqueued_us);
        ++lane.relay_messages;
        try {
            on_service(lane, msg);
        } catch (const std::exception& e) {
            note_fault(lane, msg.conn_id, e.what());
        } catch (...) {
            note_fault(lane, msg.conn_id, "?");
        }
    }
    relays.clear();

    auto& batch = lane.inbound.swap();
    if (batch.empty()) {
        // 비어 있다는 것은 자리가 있다는 뜻이다. 멈춰 둔 것이 있으면 풀어 준다
        on_after_batch(lane);
        resume_paused(lane);
        return;
    }

    const std::int64_t begin_us = core::now_us();

    // 항목마다 끝난 시각을 다음 항목의 시작으로 쓴다.
    // 항목마다 두 번 재면 그 호출 비용이 그대로 측정 대상에 섞인다
    std::int64_t last_ns = core::now_ns();

    for (auto& ev : batch) {
        const std::int64_t wait_us = begin_us - ev.enqueued_us;
        lane.queue_wait_us += static_cast<std::uint64_t>(wait_us);
        lane.queue_dist.add(wait_us);

        // 격리 단위를 항목 하나로 둔다.
        // 배치를 통째로 감싸면 예외 하나에 뒤따르던 항목이 함께 버려진다
        try {
            on_event(lane, ev);
        } catch (const std::exception& e) {
            note_fault(lane, ev.conn_id, e.what());
        } catch (...) {
            note_fault(lane, ev.conn_id, "?");
        }

        const std::int64_t done_ns = core::now_ns();
        lane.process_dist.add(done_ns - last_ns);
        last_ns = done_ns;
    }
    lane.process_us     += static_cast<std::uint64_t>(core::now_us() - begin_us);
    lane.batch_messages += batch.size();
    ++lane.batches;

    batch.clear();

    on_after_batch(lane);

    // 큐를 비웠으니 멈춰 두었던 수신을 다시 걸게 한다
    resume_paused(lane);
}

void Service::report(Lane& lane) {
    // 레인마다 자기 값을 기록한다. 다른 레인의 값을 읽으면 스레드 경계를 넘는다
    LOG_INFO("%s lane=%d faults=%llu busy=%llu | batch_avg=%.1f "
             "queue_wait=%.1fus proc=%.2fus/msg",
             name_, lane.index,
             static_cast<unsigned long long>(lane.faults),
             static_cast<unsigned long long>(lane.overflow.load(std::memory_order_relaxed)),
             lane.batches ? static_cast<double>(lane.batch_messages) /
                                static_cast<double>(lane.batches)
                          : 0.0,
             static_cast<double>(lane.queue_wait_us) /
                 static_cast<double>(lane.batch_messages ? lane.batch_messages : 1),
             static_cast<double>(lane.process_us) /
                 static_cast<double>(lane.batch_messages ? lane.batch_messages : 1));

    // 분포는 평균 옆에 두어야 쓸모가 있다.
    // 평균이 좋아 보이는 구간에서도 상단은 그 수십 배인 경우가 흔하다
    if (lane.process_dist.count() > 0) {
        LOG_INFO("%s lane=%d proc p50/p95/p99=%lld/%lld/%lldns max=%lldns | "
                 "queue p50/p95/p99=%lld/%lld/%lldus max=%lldus",
                 name_, lane.index,
                 static_cast<long long>(lane.process_dist.percentile(0.50)),
                 static_cast<long long>(lane.process_dist.percentile(0.95)),
                 static_cast<long long>(lane.process_dist.percentile(0.99)),
                 static_cast<long long>(lane.process_dist.max()),
                 static_cast<long long>(lane.queue_dist.percentile(0.50)),
                 static_cast<long long>(lane.queue_dist.percentile(0.95)),
                 static_cast<long long>(lane.queue_dist.percentile(0.99)),
                 static_cast<long long>(lane.queue_dist.max()));
    }

    // 서비스를 나눈 값어치가 이 줄에 나온다 - 다른 서비스가 넘긴 항목이
    // 큐에서 얼마나 기다렸는지가 서비스 간 전달의 실제 비용이다
    if (lane.relay_dist.count() > 0) {
        LOG_INFO("%s lane=%d relay n=%llu p50/p95/p99=%lld/%lld/%lldus max=%lldus",
                 name_, lane.index,
                 static_cast<unsigned long long>(lane.relay_messages),
                 static_cast<long long>(lane.relay_dist.percentile(0.50)),
                 static_cast<long long>(lane.relay_dist.percentile(0.95)),
                 static_cast<long long>(lane.relay_dist.percentile(0.99)),
                 static_cast<long long>(lane.relay_dist.max()));
    }

    // 밖에서 읽을 사본을 만든다. 분포를 되돌리기 전에 담아야 한다
    ServiceStats snap{};
    snap.faults   = lane.faults;
    snap.overflow = lane.overflow.load(std::memory_order_relaxed);

    // 백분위가 아니라 분포를 그대로 넘긴다. 합치는 것은 읽는 쪽이 한다
    snap.process_dist = lane.process_dist;
    snap.queue_dist   = lane.queue_dist;
    snap.relay_dist   = lane.relay_dist;

    snap.relay_messages = lane.relay_messages;

    // 파생이 자기 값을 채우고 자기 로그를 남긴다
    on_report(lane, snap);

    {
        std::lock_guard<std::mutex> guard(lane.stats_mutex);
        lane.stats = std::move(snap);
    }

    lane.batches = lane.batch_messages = lane.queue_wait_us = lane.process_us = 0;

    lane.process_dist.reset();
    lane.queue_dist.reset();
    lane.relay_dist.reset();
}

void Service::resume_paused(Lane& lane) {
    std::vector<net::ConnectionPtr> waiting;
    {
        std::lock_guard<std::mutex> guard(lane.paused_mutex);
        if (lane.paused.empty()) return;
        waiting.swap(lane.paused);
    }

    // 잠금 밖에서 깨운다. 재개는 완료 포트를 거치므로 여기서 시스템 호출이 일어나지 않는다
    for (auto& conn : waiting) {
        if (conn) conn->resume_receive();
    }
}

// -------------------------------------------------------------------
//  적재 - 전부 IOCP 워커 · DB 워커 · 다른 서비스 스레드에서 호출된다
// -------------------------------------------------------------------
void Service::post_connected(const net::ConnectionPtr& conn) {
    if (!conn || lanes_.empty()) return;
    Lane& lane = lane_of(conn->id());

    lane.inbound.emplace([&](InboundEvent& ev) {
        ev.kind        = EventKind::Connected;
        ev.conn_id     = conn->id();
        ev.msg_id      = 0;
        ev.size        = 0;
        ev.oversized   = false;
        ev.enqueued_us = core::now_us();
        ev.conn        = conn;
    });
    lane.logic.wake();
}

void Service::post_disconnected(net::ConnectionId id) {
    if (lanes_.empty()) return;
    Lane& lane = lane_of(id);

    lane.inbound.emplace([&](InboundEvent& ev) {
        ev.kind        = EventKind::Disconnected;
        ev.conn_id     = id;
        ev.msg_id      = 0;
        ev.size        = 0;
        ev.oversized   = false;
        ev.enqueued_us = core::now_us();
        ev.conn.reset();
    });
    lane.logic.wake();
}

bool Service::post_message(net::Connection& conn, const proto::FrameView& frame) {
    if (lanes_.empty()) return true;
    Lane& lane = lane_of(conn.id());

    // 큐 안의 자리에 바로 채운다. 밖에서 만들어 넘기면
    // 이동 한 번이 본문 배열 전체 복사가 된다
    const bool queued = lane.inbound.try_emplace(inbound_cap_, [&](InboundEvent& ev) {
        ev.kind        = EventKind::Message;
        ev.conn_id     = conn.id();
        ev.msg_id      = frame.msg_id;
        ev.oversized   = false;
        ev.size        = 0;
        ev.enqueued_us = core::now_us();
        ev.conn.reset();

        if (frame.size > static_cast<std::uint32_t>(kMaxEventBody)) {
            // 실을 수 없다. 판정은 서비스 스레드가 한다
            ev.oversized = true;
        } else {
            ev.size = static_cast<std::uint16_t>(frame.size);
            if (frame.size > 0) std::memcpy(ev.body.data(), frame.body, frame.size);
        }
    });

    if (!queued) {
        // 이 서비스가 밀리는 중이다. 계속 받아 두면 소비가 따라잡을 때까지 메모리만 는다.
        // 접속 · 종료는 이 경로를 타지 않는다 - 막으면 세션이 표에 남는다
        lane.overflow.fetch_add(1, std::memory_order_relaxed);

        if (policy_ == OverflowPolicy::Backpressure) {
            // 이 연결의 수신을 멈춘다. 프레임은 호출부가 버퍼에 남기고,
            // 다음 수신을 걸지 않으므로 TCP 창이 줄어 상대가 저절로 느려진다
            conn.pause_receive();
            {
                std::lock_guard<std::mutex> guard(lane.paused_mutex);
                lane.paused.push_back(conn.shared_from_this());
            }
            lane.logic.wake();
            return false;
        }

        // 거절을 알려야 클라이언트가 오지 않을 응답을 기다리지 않는다
        send_error(conn, frame.msg_id, proto::kServerBusy, "inbound queue full");
        return true;
    }
    lane.logic.wake();
    return true;
}

void Service::post_service(ServiceMessage msg) {
    // 서비스가 이미 정리된 뒤에도 상대 서비스의 종료분이 들어올 수 있다
    if (lanes_.empty()) return;

    Lane& lane = lane_of(msg.conn_id);
    lane.service_inbox.push(std::move(msg));
    lane.logic.wake();
}

} // namespace app
