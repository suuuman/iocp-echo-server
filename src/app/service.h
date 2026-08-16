#pragma once
//
//  서비스 공통 골격.
//
//  서비스 하나는 "큐 + 스레드 + 그 서비스가 단독으로 소유하는 상태" 다.
//  스레드를 기능으로 나눈다 - 같은 기능의 메시지는 어느 세션의 것이든 한 서비스로 모인다.
//  그래서 여러 세션을 한꺼번에 만지는 기능(브로드캐스트 · 매칭)이
//  그 서비스 스레드 안에서 끝나고 잠금이 필요 없다.
//
//  서비스는 남의 상태를 보지 않는다.
//  다른 세션에 무언가를 보내야 하면 자기가 들고 있는 연결 참조로 송신만 요청한다.
//  Connection::send 가 자체 잠금을 갖고 있어 어느 스레드가 불러도 안전하다.
//
//  레인 - 한 서비스 안에서 다시 나누는 단위다. 기본은 1 이다.
//         늘리면 그 서비스의 상태가 레인마다 갈라지므로,
//         세션끼리 얽히는 기능을 담은 서비스는 1 로 두어야 한다.
//         한 서비스가 병목이 됐을 때 판단할 일이다.
//
//  예외 - 항목 하나 단위로 막는다. 여기서 빠져나가면 스레드가 끝나고
//         프로세스가 함께 내려간다. 터진 연결만 끊고 나머지는 계속 처리한다.
//
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "app/inbound_event.h"
#include "core/latency.h"
#include "core/logic_thread.h"
#include "core/swap_queue.h"
#include "net/connection.h"
#include "proto/frame.h"

namespace app {

enum class ServiceId : std::uint8_t {
    Game = 0,
    Chat = 1,
    Count,
};

inline constexpr std::size_t kServiceCount = static_cast<std::size_t>(ServiceId::Count);

// 수신 큐가 가득 찼을 때 무엇을 할 것인가.
// 어느 쪽이든 메시지를 잃지는 않는다. 다른 것은 누가 기다리느냐다
enum class OverflowPolicy : std::uint8_t {
    // 그 메시지를 거절하고 오류로 답한다.
    // 상대는 계속 보낼 수 있고, 서버는 거절로 자원을 지킨다
    Reject,
    // 그 연결의 수신을 멈춘다. 프레임은 수신 버퍼에 남고
    // TCP 창이 줄어 상대의 송신이 저절로 느려진다
    Backpressure,
};

// 서비스가 다른 서비스에 넘기는 항목.
//
// 상태를 넘기지 않고 사실만 넘긴다. 받는 쪽이 그것으로 자기 표를 갱신한다.
// 이 경로가 있으면 서비스를 더 붙여도 같은 방식으로 이어진다
enum class ServiceEvent : std::uint8_t {
    // 게임 서비스가 세션 키를 발급했다. 값은 text 에 담는다
    SessionOpened,
};

struct ServiceMessage {
    ServiceEvent      kind    = ServiceEvent::SessionOpened;
    net::ConnectionId conn_id = 0;
    std::string       text;

    // 적재 시각. 서비스 간 전달에 든 시간을 재는 데 쓴다
    std::int64_t enqueued_us = 0;
};

// 밖으로 내보낼 지표.
//
// 레인 통계는 그 레인의 스레드 소유라 직접 읽을 수 없다.
// 통계 주기마다 레인이 여기에 복사해 두고, 읽는 쪽은 그 사본만 본다.
// 서비스마다 채우는 항목이 다르다 - 해당 없는 항목은 0 으로 남는다
struct ServiceStats {
    // 공통
    std::uint64_t entries  = 0;   // 그 서비스가 들고 있는 항목 수
    std::uint64_t handled  = 0;
    std::uint64_t rejected = 0;
    std::uint64_t faults   = 0;
    std::uint64_t overflow = 0;

    // 서비스 간 전달
    std::uint64_t relay_messages = 0;

    // DB (게임 서비스)
    std::uint64_t db_requests  = 0;
    std::uint64_t db_responses = 0;
    std::uint64_t db_failures  = 0;
    std::uint64_t db_expired   = 0;

    // 브로드캐스트 (채팅 서비스)
    std::uint64_t broadcasts      = 0;   // 브로드캐스트 횟수
    std::uint64_t broadcast_sends = 0;   // 그 안에서 실제로 부른 송신 횟수

    // 분포는 백분위가 아니라 **분포 자체**로 들고 다닌다.
    //
    // 서로 다른 분포의 백분위는 더하거나 평균 낼 수 없다.
    // 레인이 여럿일 때 가장 나쁜 레인의 값을 쓰면 실제보다 비관적으로 나온다.
    // 구간이 고정이므로 칸별로 더하면 합쳐진 분포가 그대로 나오고,
    // 백분위는 그 위에서 한 번만 계산하면 된다.
    //
    // 마지막 기록 주기의 값이다. 표본 수(`count()`)를 함께 내보내야
    // "빨랐던 것" 과 "잴 것이 없던 것" 을 구분할 수 있다
    core::LatencyHistogram process_dist;     // 항목 하나를 처리한 시간(ns)
    core::LatencyHistogram queue_dist;       // 적재부터 소비까지(µs)
    core::LatencyHistogram relay_dist;       // 서비스 간 전달(µs)
    core::LatencyHistogram db_round_dist;    // DB 요청 제출부터 응답까지(µs)
    core::LatencyHistogram broadcast_dist;   // 브로드캐스트 한 번(µs)
};

class Service {
public:
    struct Options {
        // 서비스 안의 레인 수. 0 이면 1 을 쓴다
        int lane_count = 0;
        // 통계 기록 주기(ms). 0 이면 기록하지 않는다
        int stats_interval_ms = 5000;
        // 레인 수신 큐의 항목 상한. 넘으면 정책에 따라 처리한다
        int inbound_queue_cap = 8192;
        // 수신 큐가 가득 찼을 때의 처리
        OverflowPolicy overflow_policy = OverflowPolicy::Reject;
    };

    explicit Service(const char* name) : name_(name) {}

    // 파생 클래스는 자기 소멸자에서 stop() 을 부른다.
    // 여기까지 내려온 뒤에 스레드를 멈추면, 그 사이 도는 스레드가
    // 이미 사라진 파생 객체의 재정의 함수를 부르게 된다
    virtual ~Service();

    Service(const Service&)            = delete;
    Service& operator=(const Service&) = delete;

    bool start(const Options& opt);
    void stop();

    // 아직 소비되지 않은 항목 수. 드레인 판정에 쓴다.
    // 큐 잠금을 잡으므로 const 가 아니다 - 큐 자체는 스레드 안전하다
    std::size_t pending();
    // 소비가 신호로 도므로 깨워 두지 않으면 유휴 상한만큼 늦어진다
    void wake_all();

    // IOCP 워커에서 호출한다.
    // post_message 는 프레임을 받아들였는지 돌려준다 -
    // false 면 호출부가 그 프레임을 수신 버퍼에 남긴다
    void post_connected(const net::ConnectionPtr& conn);
    void post_disconnected(net::ConnectionId id);
    bool post_message(net::Connection& conn, const proto::FrameView& frame);

    // 다른 서비스의 스레드에서 호출한다
    void post_service(ServiceMessage msg);

    // 모든 레인의 값을 합친다. 백분위는 레인 중 가장 나쁜 값을 쓴다 -
    // 서로 다른 분포의 백분위는 더하거나 평균 낼 수 없다
    ServiceStats stats() const;

    const char* name() const noexcept { return name_; }
    int lane_count() const noexcept { return static_cast<int>(lanes_.size()); }

    static OverflowPolicy parse_policy(const std::string& name);
    static const char*    policy_name(OverflowPolicy p);

protected:
    struct Lane {
        int               index = 0;
        core::LogicThread logic;

        core::SwapQueue<InboundEvent>   inbound;
        core::SwapQueue<ServiceMessage> service_inbox;

        // 상한에 걸려 거절한 메시지 수.
        // 적재는 IOCP 워커가 하고 기록은 서비스 스레드가 하므로 원자값으로 둔다
        std::atomic<std::uint64_t> overflow{0};

        // 아래는 전부 서비스 스레드 전용이다
        std::uint64_t faults         = 0;
        std::uint64_t batches        = 0;
        std::uint64_t batch_messages = 0;
        std::uint64_t queue_wait_us  = 0;   // 적재부터 소비까지
        std::uint64_t process_us     = 0;   // 소비 구간에서 실제로 쓴 시간
        std::uint64_t relay_messages = 0;

        // 분포. 평균이 좋아 보이는 구간에서도 상단은 그 수십 배인 경우가 흔하다.
        // 처리 시간만 나노초로 잰다 - 마이크로초 단위로는 전부 0 이 된다
        core::LatencyHistogram process_dist;   // 항목 하나를 처리한 시간(ns)
        core::LatencyHistogram queue_dist;     // 적재부터 소비까지(µs)
        core::LatencyHistogram relay_dist;     // 서비스 간 전달에 든 시간(µs)

        // 통계 주기마다 서비스 스레드가 채운다. 읽는 쪽은 관리 스레드다
        mutable std::mutex stats_mutex;
        ServiceStats       stats;

        // 수신을 멈춰 둔 연결. IOCP 워커가 넣고 서비스 스레드가 뺀다
        std::mutex                      paused_mutex;
        std::vector<net::ConnectionPtr> paused;
    };

    // 레인은 만들어졌고 스레드는 아직 돌지 않는 시점에 불린다.
    // 파생 클래스가 레인마다 자기 상태를 준비한다
    virtual bool on_created(int count) = 0;

    // 큐에서 꺼낸 항목 하나를 처리한다
    virtual void on_event(Lane& lane, InboundEvent& ev) = 0;

    // 다른 서비스가 넘긴 항목 하나를 처리한다
    virtual void on_service(Lane& lane, ServiceMessage& msg) = 0;

    // 처리 도중 예외가 나왔다. 그 연결의 상태를 정리한다
    virtual void on_fault(Lane& lane, net::ConnectionId id) = 0;

    // 통계 주기마다 불린다. 파생이 자기 값을 out 에 채우고 자기 로그를 남긴 뒤,
    // 주기값(누적이 아닌 것)을 되돌린다. 골격은 자기 몫을 알아서 되돌린다
    virtual void on_report(Lane& lane, ServiceStats& out) = 0;

    // 소비를 기다리는 파생 소유 큐의 항목 수(게임 서비스의 DB 응답 큐)
    virtual std::size_t on_pending(Lane& lane) { (void)lane; return 0; }

    // 수신 큐를 비우기 전에 불린다. 기다리는 쪽을 우선할 일이 있으면 여기서 한다
    virtual void on_before_batch(Lane& lane) { (void)lane; }

    // 수신 큐를 다 비운 뒤에 불린다. 배치가 비어 있어도 불린다.
    // 한 회차 안에서만 유효한 임시 상태를 여기서 정리한다
    virtual void on_after_batch(Lane& lane) { (void)lane; }

    // 스레드가 멈춘 뒤, 레인이 사라지기 전에 불린다.
    // 파생 상태가 레인을 참조하고 있으면 여기서 놓는다
    virtual void on_stopped() {}

    Lane& lane_at(int index) noexcept {
        return *lanes_[static_cast<std::size_t>(index)];
    }
    Lane& lane_of(net::ConnectionId id) noexcept {
        return *lanes_[static_cast<std::size_t>(id % lanes_.size())];
    }

    // 파생이 자기 큐에서 예외를 만났을 때 부른다
    void note_fault(Lane& lane, net::ConnectionId id, const char* what);

private:
    void run_step(Lane& lane);
    void report(Lane& lane);
    // 큐에 자리가 났으니 멈춰 두었던 수신을 다시 걸게 한다
    void resume_paused(Lane& lane);

    const char*                        name_;
    std::vector<std::unique_ptr<Lane>> lanes_;
    std::size_t                        inbound_cap_ = 8192;
    OverflowPolicy                     policy_      = OverflowPolicy::Reject;
};

} // namespace app
