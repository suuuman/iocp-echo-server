#pragma once
//
//  완료 포트와 워커 스레드를 보유한다.
//
//  워커가 하는 일은 통보 수거와 분기까지다.
//  게임 로직 · DB 호출 · 파일 I/O 는 이 스레드에서 수행하지 않는다.
//  M2 에서 메시지는 로직 스레드 큐로 넘어간다.
//
#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "net/connection.h"

namespace net {

class IoService {
public:
    struct Options {
        // 0 이면 코어수/2 로 산정하고 하한 2 · 상한 4 를 적용한다
        int  worker_count = 0;
        // 무응답 판정 시간(ms). 0 이면 판정하지 않는다
        int  heartbeat_timeout_ms = 30000;
        // 회수 · 만료 검사 주기(ms)
        int  sweep_interval_ms = 1000;
        // 동시 접속 상한. 0 이면 제한하지 않는다.
        // 접속 하나가 수신 버퍼와 송신 누적분을 들고 있으므로
        // 상한이 없으면 접속 수가 그대로 메모리 사용량이 된다
        int  max_connections = 8192;
        // 연결 하나가 보낼 수 있는 초당 메시지 수. 0 이면 제한하지 않는다
        int  message_rate_limit = 0;
        // 잠깐 몰리는 몫. 0 이면 상한과 같게 둔다
        int  message_burst = 0;
        // 아직 내보내지 못한 송신 누적분의 연결당 상한(바이트). 0 이면 제한하지 않는다.
        // 응답을 읽지 않는 상대에게는 이 값이 유일한 방어선이다
        int  send_buffer_limit = 256 * 1024;

        // 발행을 누가 하는가.
        //
        //   false(통지) 보내는 쪽은 완료 포트에 통지만 걸고 워커가 WSASend 를 부른다.
        //               보내는 쪽 스레드에 송신 비용이 실리지 않는다.
        //               다만 통지 자체가 시스템 호출이라, 쌓이는 것이 없으면
        //               메시지 하나에 시스템 호출이 두 번 든다
        //   true(직접)  보내는 쪽이 그 자리에서 WSASend 를 부른다.
        //               시스템 호출이 한 번으로 줄고 완료 포트 왕복이 사라지는 대신,
        //               발행 비용이 보내는 쪽 스레드의 처리량 상한에 들어간다
        bool direct_send = false;
    };

    // 수락 완료 통보는 Acceptor 가 받는다.
    // IoService 는 작업 종류만 보고 넘긴다
    using AcceptHandler  = std::function<void(AcceptOp&, bool ok)>;
    using ConnectHandler = std::function<void(ConnectOp&, bool ok)>;

    IoService() = default;
    ~IoService();

    IoService(const IoService&)            = delete;
    IoService& operator=(const IoService&) = delete;

    bool start(const Options& opt);
    void stop();

    // 송신 대기분이 소켓으로 나갈 때까지 기다린다.
    // 이걸 건너뛰고 닫으면 마지막 응답이 상대에게 도달하지 못한다
    void flush_pending(int timeout_ms);

    // 소켓을 완료 포트에 등록한다. 완료 키로 연결 식별자를 넘긴다
    bool bind_socket(SOCKET s, ConnectionId id);

    // 커널 입출력 없이 완료 포트에 통지만 밀어 넣는다.
    // 로직 스레드가 시스템 호출을 직접 하지 않게 하는 통로다
    bool post_flush(Connection& conn, FlushOp& op);
    bool post_resume(Connection& conn, ResumeOp& op);

    // 식별자를 미리 발급한다.
    // 소켓은 완료 포트에 한 번만 붙일 수 있고 완료 키도 그때 정해진다.
    // 거는 쪽은 접속 전에 붙여야 하므로 식별자가 먼저 필요하다
    ConnectionId reserve_id();

    // 소켓을 연결로 승격시킨다.
    // setup 은 완료 포트 등록 후 첫 수신을 걸기 전에 호출된다.
    // 핸들러 등록과 접속 통지를 여기서 마쳐야 첫 메시지보다 앞선다.
    // bound_id 가 0 이 아니면 이미 등록된 소켓으로 보고 등록을 건너뛴다
    using SetupHandler = std::function<void(Connection&)>;
    ConnectionPtr adopt(SOCKET s, std::string peer, const SetupHandler& setup,
                        ConnectionId bound_id = 0);

    ConnectionPtr find(ConnectionId id);
    std::size_t   connection_count();

    // 상한에 걸려 되돌린 접속 수
    std::uint64_t refused() const noexcept {
        return refused_.load(std::memory_order_relaxed);
    }

    // 속도 제한에 걸린 메시지 수. 판정은 호출부가 하고 집계만 여기서 한다
    void count_throttled() noexcept {
        throttled_.fetch_add(1, std::memory_order_relaxed);
    }
    std::uint64_t throttled() const noexcept {
        return throttled_.load(std::memory_order_relaxed);
    }

    // 송신 누적 상한에 걸려 끊은 연결 수. 0 이 아니면 응답을 읽지 않는 상대가 있다는 뜻이다
    void count_send_overflow() noexcept {
        send_overflow_.fetch_add(1, std::memory_order_relaxed);
    }
    std::uint64_t send_overflow() const noexcept {
        return send_overflow_.load(std::memory_order_relaxed);
    }

    // 발행 통지 수 · 실제 발행 수.
    //
    // 통지는 보내는 쪽 스레드에서 완료 포트에 거는 시스템 호출이고,
    // 발행은 워커가 부르는 WSASend 다. 둘의 비가 통지 하나에 몇 건이 실려 나갔는지다.
    // 이 값이 1 에 가까우면 메시지마다 시스템 호출이 두 번 드는 것이고,
    // 크면 쌓인 것들이 한 번에 나가고 있다는 뜻이다
    void count_flush_post() noexcept {
        flush_posts_.fetch_add(1, std::memory_order_relaxed);
    }
    void count_send_issued(std::uint64_t messages) noexcept {
        send_issues_.fetch_add(1, std::memory_order_relaxed);
        send_messages_.fetch_add(messages, std::memory_order_relaxed);
    }
    std::uint64_t flush_posts() const noexcept {
        return flush_posts_.load(std::memory_order_relaxed);
    }
    std::uint64_t send_issues() const noexcept {
        return send_issues_.load(std::memory_order_relaxed);
    }
    std::uint64_t send_messages() const noexcept {
        return send_messages_.load(std::memory_order_relaxed);
    }

    // 전량이 나가지 못해 남은 구간을 다시 실은 횟수.
    // 중첩 송신에서는 드물지만 0 이라는 보장이 없어 경로를 갖춰 둔다
    void count_partial_send() noexcept {
        partial_sends_.fetch_add(1, std::memory_order_relaxed);
    }
    std::uint64_t partial_sends() const noexcept {
        return partial_sends_.load(std::memory_order_relaxed);
    }

    void set_accept_handler(AcceptHandler h)   { accept_handler_  = std::move(h); }
    void set_connect_handler(ConnectHandler h) { connect_handler_ = std::move(h); }

    HANDLE port() const noexcept { return iocp_; }
    bool   running() const noexcept { return running_.load(std::memory_order_acquire); }

    static int decide_worker_count(int requested);

private:
    void worker_loop();
    void sweep_loop();     // 만료 · 회수 담당
    void dispatch(ConnectionId id, IoOperation* op, int transferred, bool ok);

    HANDLE iocp_ = nullptr;
    Options opt_{};

    std::vector<std::thread> workers_;
    std::thread              sweeper_;
    std::atomic<bool>        running_{false};

    AcceptHandler  accept_handler_;
    ConnectHandler connect_handler_;

    std::mutex                                          mutex_;
    std::unordered_map<ConnectionId, ConnectionPtr>     connections_;
    std::atomic<ConnectionId>                           next_id_{1};
    std::atomic<std::uint64_t>                          refused_{0};
    std::atomic<std::uint64_t>                          throttled_{0};
    std::atomic<std::uint64_t>                          send_overflow_{0};
    std::atomic<std::uint64_t>                          flush_posts_{0};
    std::atomic<std::uint64_t>                          send_issues_{0};
    std::atomic<std::uint64_t>                          send_messages_{0};
    std::atomic<std::uint64_t>                          partial_sends_{0};
};

} // namespace net
