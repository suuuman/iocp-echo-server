//
//  서버 진입점.
//
//  스레드 구성
//    IOCP 워커 (2~4) : 완료 통보 수거 → 메시지 경계 복원 → 서비스 큐 적재. 상태 접근 없음
//    게임 서비스 (1)  : 세션 표 · Echo · Save · History · Counter · Ping
//    채팅 서비스 (1)  : 접속자 목록 · Chat · 브로드캐스트
//    DB 워커 (N)      : 질의 실행. 세션 키 해시로 배정되어 순서가 보장된다
//    회수 스레드 (1)  : 만료 판정 · 미완료 0인 연결 회수
//    기록 스레드 (1)  : 로그 파일 쓰기
//
//  서비스는 자기 상태를 단독으로 소유한다. 그래서 게임 상태에 잠금이 없다.
//
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <windows.h>

#include "app/metrics.h"
#include "app/reply.h"
#include "app/service_router.h"
#include "core/clock.h"
#include "core/ini_reader.h"
#include "core/log.h"
#include "db/db_worker_pool.h"
#include "net/acceptor.h"
#include "net/admin_server.h"
#include "net/io_service.h"
#include "proto/frame.h"

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI console_handler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            g_stop.store(true, std::memory_order_release);
            return TRUE;
        default:
            return FALSE;
    }
}

const char* arg_value(int argc, char** argv, const char* name) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    }
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    ::SetConsoleCtrlHandler(console_handler, TRUE);

    // ---------------------------------------------------------------
    //  설정 - 로그 회전 값을 여기서 읽으므로 기록보다 먼저 온다
    // ---------------------------------------------------------------
    const char* config_path = arg_value(argc, argv, "--config");
    core::IniReader ini;
    const bool ini_loaded = ini.load(config_path ? config_path : "config/db.ini");

    core::log_start("logs/server.log", core::LogLevel::Info,
                    static_cast<long long>(ini.get_int("log", "max_mb", 64)) * 1024 * 1024,
                    ini.get_int("log", "keep_files", 10));

    if (!ini_loaded) LOG_WARN("%s - 기본값으로 진행한다", ini.error().c_str());

    // 스레드를 만들기 전에 부른다. 워커 여러 개가 동시에 처음 접속하면
    // 라이브러리 내부 초기화가 경합해 Winsock 준비 전에 접속을 시도한다
    if (!db::library_init()) {
        LOG_ERROR("mysql library init failed");
        core::log_stop();
        return 1;
    }

    db::ConnectionConfig db_cfg;
    db_cfg.host       = ini.get("mysql", "host", "127.0.0.1");
    db_cfg.port       = static_cast<unsigned>(ini.get_int("mysql", "port", 3306));
    db_cfg.user       = ini.get("mysql", "user", "echo");
    db_cfg.password   = ini.get("mysql", "password", "");
    db_cfg.schema     = ini.get("mysql", "schema", "echodb");
    db_cfg.timeout_ms = static_cast<unsigned>(ini.get_int("mysql", "timeout_ms", 5000));
    // 같은 일을 직접 SQL 로도, 저장 프로시저로도 할 수 있다.
    // 왕복 횟수를 비교하려고 둘 다 남겨 두었다
    db_cfg.use_procedures = ini.get_int("mysql", "use_procedures", 0) != 0;

    const int db_workers = ini.get_int("mysql", "worker_count", 4);

    // 워커 배정 방식. 같은 세션 키의 순서는 어느 쪽이든 보장된다
    const db::Dispatch db_dispatch =
        db::WorkerPool::parse_dispatch(ini.get("mysql", "dispatch", "static"));

    // 한 트랜잭션에 묶을 Save 의 최대 건수. 1 이면 묶지 않는다
    const int db_batch_size = ini.get_int("mysql", "batch_size", 1);

    // 종료 시 남은 일을 마칠 때까지 기다리는 상한(ms)
    const int drain_ms = ini.get_int("server", "shutdown_drain_ms", 5000);

    // 관리 포트. 0 이면 열지 않는다.
    // 한 장비에 인스턴스를 둘 이상 띄우면 두 번째의 bind 가 실패하므로
    // 수신 포트와 마찬가지로 명령행으로도 바꿀 수 있어야 한다
    auto admin_port = static_cast<std::uint16_t>(ini.get_int("server", "admin_port", 9100));
    if (const char* a = arg_value(argc, argv, "--admin-port")) {
        admin_port = static_cast<std::uint16_t>(std::atoi(a));
    }

    // 관리 포트를 열지 못했을 때 기동을 멈출지.
    // 지표를 못 읽는 상태로 도는 것을 눈치채지 못하는 편이 더 위험한 운영도 있다
    const bool admin_required = ini.get_int("server", "admin_port_required", 0) != 0;

    std::uint16_t port = static_cast<std::uint16_t>(ini.get_int("server", "listen_port", 9000));
    if (const char* p = arg_value(argc, argv, "--port")) {
        port = static_cast<std::uint16_t>(std::atoi(p));
    }

    net::IoService::Options io_opt{};
    io_opt.worker_count         = ini.get_int("server", "io_worker_count", 0);
    io_opt.heartbeat_timeout_ms = ini.get_int("server", "heartbeat_timeout_ms", 30000);
    io_opt.sweep_interval_ms    = 1000;
    io_opt.max_connections      = ini.get_int("server", "max_connections", 8192);
    io_opt.message_rate_limit   = ini.get_int("server", "message_rate_limit", 0);
    io_opt.message_burst        = ini.get_int("server", "message_burst", 0);
    io_opt.send_buffer_limit    = ini.get_int("server", "send_buffer_limit_kb", 256) * 1024;
    // notify - 완료 포트에 통지만 걸고 워커가 발행한다
    // direct - 보내는 쪽이 그 자리에서 발행한다
    io_opt.direct_send          = ini.get("server", "send_mode", "notify") == "direct";

    app::ServiceRouter::Options svc_opt{};
    // 서비스 안에서 다시 나누는 값이다. 기본은 1 이다
    svc_opt.game_lane_count   = ini.get_int("server", "game_lane_count", 0);
    svc_opt.stats_interval_ms = 5000;
    svc_opt.inbound_queue_cap = ini.get_int("server", "inbound_queue_cap", 8192);
    svc_opt.db_request_timeout_ms = ini.get_int("mysql", "request_timeout_ms", 3000);
    svc_opt.overflow_policy =
        app::Service::parse_policy(ini.get("server", "inbound_full_policy", "reject"));
    // owner - 소속 검사로 지킨다 · mutex - 잠금으로 지킨다
    svc_opt.session_guard =
        app::SessionTable::parse_guard(ini.get("server", "session_table_guard", "owner"));

    // ---------------------------------------------------------------
    //  구성
    // ---------------------------------------------------------------
    db::WorkerPool     db_pool;
    app::ServiceRouter services;
    net::IoService     io;

    if (!services.start(svc_opt, &db_pool)) {
        core::log_stop();
        return 1;
    }

    // DB 응답은 요청을 낸 레인으로 되돌린다
    db_pool.start(db_cfg, db_workers, [&services](std::unique_ptr<db::Response> res) {
        services.post_db_response(std::move(res));
    }, db_dispatch, db_batch_size);

    if (!io.start(io_opt)) {
        db_pool.stop();
        services.stop();
        core::log_stop();
        return 1;
    }

    // ---------------------------------------------------------------
    //  IOCP 워커에서 실행되는 부분 - 서비스 큐 적재까지만 한다
    // ---------------------------------------------------------------
    auto on_message = [&services, &io](net::Connection& conn, const proto::FrameView& frame) {
        // 속도 제한은 큐에 넣기 전에 본다.
        // 적재한 뒤에 걸러 내면 폭주 연결의 메시지가 이미 큐를 차지한 뒤다
        if (!conn.allow_message()) {
            io.count_throttled();
            app::send_error(conn, frame.msg_id, proto::kServerBusy, "rate limit");
            return true;   // 답을 보냈으므로 이 프레임은 소비된 것이다
        }
        return services.post_message(conn, frame);
    };
    // 종료는 모든 서비스가 알아야 한다. 서비스마다 자기 목록이 따로다
    auto on_close = [&services](net::Connection& conn) {
        services.post_disconnected(conn.id());
    };

    net::Acceptor acceptor(io);
    const bool listening = acceptor.start(port, [&](SOCKET s, std::string peer) {
        io.adopt(s, std::move(peer), [&](net::Connection& conn) {
            conn.set_message_handler(on_message);
            conn.set_close_handler(on_close);

            // 접속 통지를 첫 수신보다 먼저 큐에 넣는다
            services.post_connected(conn.shared_from_this());
        });
    });

    if (!listening) {
        io.stop();
        db_pool.stop();
        services.stop();
        core::log_stop();
        return 1;
    }

    // ---------------------------------------------------------------
    //  관리 포트 - 감시 도구가 볼 지점
    // ---------------------------------------------------------------
    const std::int64_t started_ms = core::now_ms();

    net::AdminServer admin;
    const bool admin_ready = admin.start(
        admin_port,
        [&]() {
            // 종료 중이면 새 트래픽을 받지 않는 편이 낫다.
            // 로드밸런서가 이 값을 보고 미리 빼 준다
            if (g_stop.load(std::memory_order_acquire)) return false;
            // 워커를 두고도 하나도 붙지 못했다면 DB 를 쓰는 요청은 전부 실패한다
            if (db_workers > 0 && db_pool.connected_workers() == 0) return false;
            return true;
        },
        [&]() {
            return app::render_metrics(io, services, db_pool, core::now_ms() - started_ms);
        });

    if (!admin_ready && admin_required) {
        LOG_ERROR("admin port=%u 를 열지 못했다. admin_port_required 가 켜져 있어 기동을 멈춘다",
                  admin_port);
        acceptor.stop();
        io.stop();
        db_pool.stop();
        services.stop();
        core::log_stop();
        return 1;
    }

    LOG_INFO("echo server ready port=%u admin=%u game_lanes=%d db_workers=%d db_path=%s db_dispatch=%s"
             " send_mode=%s. press ctrl+c to stop",
             port, admin_port, services.game_lane_count(), db_workers,
             db_cfg.use_procedures ? "procedure" : "direct",
             db::WorkerPool::dispatch_name(db_dispatch),
             io_opt.direct_send ? "direct" : "notify");

    std::int64_t last_report = core::now_ms();
    while (!g_stop.load(std::memory_order_acquire)) {
        ::Sleep(200);

        const std::int64_t now = core::now_ms();
        if (now - last_report >= 5000) {
            last_report = now;
            LOG_INFO("connections=%zu refused=%llu throttled=%llu send_overflow=%llu "
                     "flush_posts=%llu sends=%llu msgs_per_send=%.2f "
                     "db_queued=%zu db_conn=%d/%d dropped_logs=%llu",
                     io.connection_count(),
                     static_cast<unsigned long long>(io.refused()),
                     static_cast<unsigned long long>(io.throttled()),
                     static_cast<unsigned long long>(io.send_overflow()),
                     static_cast<unsigned long long>(io.flush_posts()),
                     static_cast<unsigned long long>(io.send_issues()),
                     io.send_issues() ? static_cast<double>(io.send_messages()) /
                                            static_cast<double>(io.send_issues())
                                      : 0.0,
                     db_pool.queued(),
                     db_pool.connected_workers(), db_pool.worker_count(),
                     core::log_dropped());
        }
    }

    LOG_INFO("shutting down");
    acceptor.stop();   // 새 접속을 먼저 막는다. 그래야 남은 일이 늘지 않는다

    // 받아 둔 일을 마치고 응답이 소켓으로 나간 뒤에 연결을 닫는다.
    // 순서가 반대면 응답을 만들어도 받을 상대가 남아 있지 않다
    db_pool.drain(drain_ms);      // 큐에 남은 질의를 기한까지 처리한다
    services.drain(drain_ms);     // 서비스가 그 응답을 소비해 송신에 싣는다
    io.flush_pending(drain_ms);   // 송신분이 실제로 나갈 때까지 기다린다

    // 드레인이 끝날 때까지는 관리 포트를 살려 둔다.
    // 그동안 /health 가 503 을 답해 로드밸런서가 트래픽을 빼 간다
    admin.stop();

    io.stop();        // 연결을 닫아 종료 통지를 서비스 큐에 밀어 넣는다
    db_pool.stop();   // 기한을 넘긴 잔여 요청에 응답하고 커넥션을 닫는다
    services.stop();  // 남은 항목을 비우고 끝낸다
    db::library_shutdown();
    core::log_stop();
    return 0;
}
