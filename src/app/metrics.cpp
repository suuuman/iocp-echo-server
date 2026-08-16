#include "app/metrics.h"

#include <cstdio>

#include "core/log.h"

namespace app {
namespace {

void line(std::string& out, const char* name, const char* help, const char* kind,
          unsigned long long value) {
    char buffer[512];
    const int n = std::snprintf(buffer, sizeof(buffer),
                                "# HELP %s %s\n# TYPE %s %s\n%s %llu\n",
                                name, help, name, kind, name, value);
    if (n > 0) out.append(buffer, static_cast<std::size_t>(n));
}

void counter(std::string& out, const char* name, const char* help, unsigned long long value) {
    line(out, name, help, "counter", value);
}

void gauge(std::string& out, const char* name, const char* help, unsigned long long value) {
    line(out, name, help, "gauge", value);
}

// 백분위는 이름이 같고 라벨만 다른 여러 줄로 나간다.
// 설명과 종류는 한 번만 쓴다 - 같은 이름에 두 번 쓰면 형식 위반이다.
//
// 표본 수를 함께 내보낸다. 백분위는 마지막 기록 주기의 값이라
// 그 주기에 트래픽이 없으면 0 이 되는데, 이 값이 없으면
// "빨랐던 것" 과 "잴 것이 없던 것" 을 구분할 수 없다
// 분포에서 바로 낸다. 백분위를 미리 계산해 넘기면 여러 레인의 값을 합칠 수 없다 -
// 서로 다른 분포의 백분위는 더하거나 평균 낼 수 없기 때문이다.
// 합치는 것은 분포 쪽에서 끝내고, 여기서는 한 번만 계산한다
void quantiles(std::string& out, const char* name, const char* help,
               const char* service, const core::LatencyHistogram& dist) {
    char buffer[768];
    const int n = std::snprintf(buffer, sizeof(buffer),
                                "%s{service=\"%s\",quantile=\"0.5\"} %lld\n"
                                "%s{service=\"%s\",quantile=\"0.99\"} %lld\n"
                                "%s_samples{service=\"%s\"} %llu\n",
                                name, service,
                                static_cast<long long>(dist.percentile(0.50)),
                                name, service,
                                static_cast<long long>(dist.percentile(0.99)),
                                name, service,
                                static_cast<unsigned long long>(dist.count()));
    if (n > 0) out.append(buffer, static_cast<std::size_t>(n));
    (void)help;
}

// 설명과 종류는 같은 이름에 한 번만 쓴다. 두 번 쓰면 형식 위반이다
void quantile_head(std::string& out, const char* name, const char* help) {
    char buffer[512];
    const int n = std::snprintf(buffer, sizeof(buffer),
                                "# HELP %s %s\n# TYPE %s gauge\n"
                                "# HELP %s_samples 마지막 기록 주기의 표본 수\n"
                                "# TYPE %s_samples gauge\n",
                                name, help, name, name, name);
    if (n > 0) out.append(buffer, static_cast<std::size_t>(n));
}

} // namespace

std::string render_metrics(net::IoService& io, const ServiceRouter& services,
                           const db::WorkerPool& db, std::int64_t uptime_ms) {
    const ServiceStats s = services.stats();

    std::string out;
    out.reserve(4096);

    gauge(out, "echo_uptime_seconds", "기동 후 경과 시간",
          static_cast<unsigned long long>(uptime_ms / 1000));

    // 접속
    gauge(out, "echo_connections", "현재 접속 수",
          static_cast<unsigned long long>(io.connection_count()));
    counter(out, "echo_connections_refused_total", "상한에 걸려 되돌린 접속 수",
            static_cast<unsigned long long>(io.refused()));
    gauge(out, "echo_sessions", "서비스가 들고 있는 항목 수의 합", s.entries);

    // 메시지
    counter(out, "echo_messages_handled_total", "처리한 메시지 수", s.handled);
    counter(out, "echo_messages_rejected_total", "오류로 응답한 메시지 수", s.rejected);
    counter(out, "echo_inbound_overflow_total", "수신 큐 상한에 걸려 거절한 메시지 수", s.overflow);
    counter(out, "echo_messages_throttled_total", "속도 제한에 걸린 메시지 수",
            static_cast<unsigned long long>(io.throttled()));
    counter(out, "echo_send_overflow_total", "송신 누적 상한에 걸려 끊은 연결 수",
            static_cast<unsigned long long>(io.send_overflow()));

    // 발행 통지와 실제 발행. 통지는 보내는 쪽 스레드의 시스템 호출이고
    // 발행은 워커의 WSASend 다. messages/issues 가 발행 하나에 실린 건수다
    counter(out, "echo_flush_posts_total", "발행 통지 수",
            static_cast<unsigned long long>(io.flush_posts()));
    counter(out, "echo_send_issues_total", "실제 발행(WSASend) 수",
            static_cast<unsigned long long>(io.send_issues()));
    counter(out, "echo_send_messages_total", "발행에 실려 나간 메시지 수",
            static_cast<unsigned long long>(io.send_messages()));
    counter(out, "echo_partial_sends_total", "전량이 나가지 못해 남은 구간을 다시 실은 횟수",
            static_cast<unsigned long long>(io.partial_sends()));
    counter(out, "echo_handler_faults_total", "처리 중 예외로 끊은 연결 수", s.faults);

    // DB
    counter(out, "echo_db_requests_total", "DB 요청 수", s.db_requests);
    counter(out, "echo_db_responses_total", "DB 응답 수", s.db_responses);
    counter(out, "echo_db_failures_total", "실패로 끝난 DB 요청 수", s.db_failures);
    counter(out, "echo_db_expired_total", "기한을 넘겨 실행하지 않은 DB 요청 수", s.db_expired);
    gauge(out, "echo_db_queued", "DB 큐에 남은 요청 수",
          static_cast<unsigned long long>(db.queued()));
    gauge(out, "echo_db_workers", "DB 워커 수",
          static_cast<unsigned long long>(db.worker_count()));
    gauge(out, "echo_db_workers_connected", "커넥션이 살아 있는 DB 워커 수",
          static_cast<unsigned long long>(db.connected_workers()));

    // 브로드캐스트 - 채팅 1건이 접속자 전원에게 나가는 경로다.
    // 송신 수를 함께 내보내야 "몇 명에게 퍼졌는지" 를 알 수 있다
    counter(out, "echo_broadcasts_total", "브로드캐스트 횟수", s.broadcasts);
    counter(out, "echo_broadcast_sends_total", "브로드캐스트가 부른 송신 횟수",
            s.broadcast_sends);

    // 서비스 간 전달 - 서비스를 나눈 비용이 이 값에 나온다
    counter(out, "echo_service_relays_total", "서비스 간 전달로 받은 항목 수",
            s.relay_messages);

    // 분포 - 서비스마다 따로 낸다.
    //
    // 한 서비스 안에서는 레인들의 분포를 칸별로 합친 뒤 백분위를 한 번만 계산한다.
    // 서비스끼리는 합치지 않는다 - "항목 하나" 가 뜻하는 일이 다르기 때문이다.
    // 게임의 한 건은 메시지 하나이고, 채팅의 한 건은 접속자 전원에게 보내는 일이다
    const ServiceStats game = services.game_stats();
    const ServiceStats chat = services.chat_stats();

    quantile_head(out, "echo_process_nanoseconds", "항목 하나를 처리한 시간");
    quantiles(out, "echo_process_nanoseconds", nullptr, "game", game.process_dist);
    quantiles(out, "echo_process_nanoseconds", nullptr, "chat", chat.process_dist);

    quantile_head(out, "echo_queue_wait_microseconds", "적재부터 소비까지 기다린 시간");
    quantiles(out, "echo_queue_wait_microseconds", nullptr, "game", game.queue_dist);
    quantiles(out, "echo_queue_wait_microseconds", nullptr, "chat", chat.queue_dist);

    quantile_head(out, "echo_service_relay_microseconds", "서비스 간 전달에 든 시간");
    quantiles(out, "echo_service_relay_microseconds", nullptr, "game", game.relay_dist);
    quantiles(out, "echo_service_relay_microseconds", nullptr, "chat", chat.relay_dist);

    quantile_head(out, "echo_db_roundtrip_microseconds", "DB 요청 제출부터 응답까지");
    quantiles(out, "echo_db_roundtrip_microseconds", nullptr, "game", game.db_round_dist);

    quantile_head(out, "echo_broadcast_microseconds", "브로드캐스트 한 번에 든 시간");
    quantiles(out, "echo_broadcast_microseconds", nullptr, "chat", chat.broadcast_dist);

    // 기록
    counter(out, "echo_log_dropped_total", "큐가 가득 차 버린 로그 줄 수",
            core::log_dropped());

    return out;
}

} // namespace app
