#pragma once
//
//  메시지 ID 로 서비스를 고른다.
//
//  IOCP 워커가 경계를 복원한 뒤 여기서 갈 곳이 정해진다.
//  표는 기동 시 한 번 만들고 그 뒤로는 읽기만 한다 - 잠금이 없는 이유다.
//
//  접속 · 종료는 모든 서비스에 보낸다. 서비스마다 자기 목록이 따로이므로
//  한 곳만 알려 주면 나머지가 유령 항목을 들고 있게 된다.
//
//  서비스 간 전달도 여기를 지난다. 서비스가 상대를 직접 알면 붙일 때마다
//  서로를 참조해야 하고, 그러면 서비스를 늘리는 비용이 제곱으로 는다.
//
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "app/broadcast_service.h"
#include "app/game_service.h"
#include "app/service.h"
#include "db/db_types.h"
#include "db/db_worker_pool.h"
#include "net/connection.h"
#include "proto/frame.h"

namespace app {

class ServiceRouter {
public:
    struct Options {
        // 게임 서비스의 레인 수. 0 이면 1
        int game_lane_count = 0;
        int stats_interval_ms = 5000;
        int inbound_queue_cap = 8192;
        int db_request_timeout_ms = 3000;
        OverflowPolicy overflow_policy = OverflowPolicy::Reject;
        // 세션 표를 무엇으로 지키는가. 소속 검사 또는 잠금
        SessionGuard session_guard = SessionGuard::Owner;
    };

    bool start(const Options& opt, db::WorkerPool* db);
    void stop();

    // 큐에 남은 항목을 기한까지 소비한다.
    // 응답이 큐에 남은 채로 연결을 닫으면 기다리던 쪽은 아무것도 받지 못한다
    void drain(int timeout_ms);

    // IOCP 워커에서 호출한다
    void post_connected(const net::ConnectionPtr& conn);
    void post_disconnected(net::ConnectionId id);
    bool post_message(net::Connection& conn, const proto::FrameView& frame);

    // DB 워커에서 호출한다
    void post_db_response(std::unique_ptr<db::Response> res);

    // 서비스 스레드에서 호출한다
    void post_to(ServiceId target, ServiceMessage msg);

    // 모든 서비스의 **개수**를 합친다. 분포는 합치지 않는다 -
    // 서비스마다 "항목 하나" 가 뜻하는 일이 다르기 때문이다
    ServiceStats stats() const;

    // 분포는 서비스마다 따로 읽는다
    ServiceStats game_stats() const { return game_.stats(); }
    ServiceStats chat_stats() const { return chat_.stats(); }

    int game_lane_count() const noexcept { return game_.lane_count(); }

private:
    // 표는 요청 ID 만 담는다. 응답 번호(100 이상)는 서버가 받지 않는다
    static constexpr std::size_t kRouteTableSize = 100;
    static constexpr std::uint8_t kNoService     = 0xFF;

    Service* route(std::uint16_t msg_id) noexcept;
    Service* service_at(std::size_t index) noexcept;

    GameService      game_;
    BroadcastService chat_;

    std::array<std::uint8_t, kRouteTableSize> table_{};

    // 어느 서비스도 맡지 않은 메시지 수. IOCP 워커가 올린다
    std::atomic<std::uint64_t> unknown_{0};
};

} // namespace app
