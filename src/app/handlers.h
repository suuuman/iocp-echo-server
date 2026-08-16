#pragma once
//
//  게임 서비스의 메시지 처리. 전부 그 서비스의 스레드에서 실행된다.
//
//  DB 가 필요한 처리는 요청만 넣고 즉시 반환한다.
//  응답이 오면 서비스 스레드가 다시 집어 이어서 처리한다.
//
#include <cstdint>
#include <functional>
#include <memory>

#include "app/inbound_event.h"
#include "app/service.h"
#include "app/session_table.h"
#include "core/latency.h"
#include "core/logic_thread.h"
#include "db/db_types.h"
#include "db/db_worker_pool.h"
#include "proto/messages.h"

namespace app {

// 처리에 필요한 것들을 한 자리에 묶는다
struct AppContext {
    core::LogicThread* logic    = nullptr;
    SessionTable*      sessions = nullptr;
    db::WorkerPool*    db       = nullptr;

    // DB 요청 하나에 주는 시간(µs). 0 이면 기한을 두지 않는다
    std::int64_t db_deadline_us = 0;

    // 세션 키가 나왔음을 다른 서비스에 알린다. 값만 넘기고 표는 넘기지 않는다
    std::function<void(ServiceMessage)> relay;

    // 통계
    std::uint64_t handled  = 0;
    std::uint64_t rejected = 0;

    // DB 왕복
    std::uint64_t db_requests   = 0;
    std::uint64_t db_responses  = 0;    // 기록 주기마다 0 으로 되돌린다
    std::uint64_t db_total_us   = 0;    // 워커가 질의에 쓴 시간
    std::uint64_t db_failures   = 0;
    // 기한을 넘겨 실행하지 않은 요청 수. 밀리고 있다는 신호다
    std::uint64_t db_expired    = 0;
    // 밖으로 내보내는 값은 누적이어야 한다. 주기값으로는 증가율을 낼 수 없다
    std::uint64_t db_responses_total = 0;

    // 분포. 큐 대기와 처리 시간은 서비스 골격이 잰다.
    // 여기서는 이 서비스만 겪는 구간을 잰다
    core::LatencyHistogram db_query_dist;   // 워커가 질의에 쓴 시간(µs)
    core::LatencyHistogram db_round_dist;   // 요청 제출부터 응답 처리까지(µs)
};

// 큐에서 꺼낸 항목 하나를 처리한다
void process_event(AppContext& ctx, InboundEvent& ev);

// DB 응답 하나를 처리한다
void process_db_response(AppContext& ctx, db::Response& res);

} // namespace app
