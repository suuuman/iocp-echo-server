#pragma once
//
//  소켓 하나와 그에 걸린 미완료 작업을 보유한다.
//
//  수명 규칙
//      소켓 종료와 객체 해제를 분리한다.
//      close() 는 소켓만 닫고, 해제는 미완료 작업이 0이 된 뒤에 이루어진다.
//      끊긴 뒤에도 커널에 발행된 요청은 완료 통보로 되돌아오기 때문이다.
//
//  송신 규칙
//      연결당 발행은 항상 1건이다. 발행 중에 쌓인 것들은 다음 한 번에 병합된다.
//      이 규칙이 전송 순서를 보장한다.
//
#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "net/io_operation.h"
#include "net/rate_limiter.h"
#include "net/receive_buffer.h"
#include "proto/frame.h"

namespace net {

class IoService;

using ConnectionId = std::uint64_t;

// 부분 전송 뒤 남은 구간을 누적분 앞에 되돌린다.
//
// 중첩 송신은 대체로 전량을 보내고 완료되지만 그것이 보장은 아니다.
// 전량이 아니라고 연결을 끊으면 보내던 데이터를 잃는다.
//
// 남은 구간이 **발행 중에 쌓인 것보다 앞에** 서야 전송 순서가 보존된다.
// 앞에 끼워 넣는 비용이 들지만 이 경로는 드물다 -
// 흔한 경로(전량 전송)에는 아무것도 더하지 않는다
inline void requeue_unsent(std::vector<char>& pending,
                           const std::vector<char>& staged,
                           std::size_t sent) {
    if (sent >= staged.size()) return;

    pending.insert(pending.begin(),
                   staged.begin() + static_cast<std::ptrdiff_t>(sent),
                   staged.end());
}

class Connection : public std::enable_shared_from_this<Connection> {
public:
    // 완성된 프레임 1개마다 호출된다.
    // body 는 수신 버퍼 내부를 가리키므로 호출이 끝나기 전에 소비를 마쳐야 한다.
    //
    // false 를 돌려주면 "지금은 받을 수 없다" 는 뜻이다.
    // 그 프레임은 소비하지 않고 버퍼에 남겨 두었다가 다시 걸릴 때 이어서 읽는다.
    // 버리면 기다리는 쪽이 오지 않을 응답을 기다린다
    using MessageHandler = std::function<bool(Connection&, const proto::FrameView&)>;
    using CloseHandler   = std::function<void(Connection&)>;

    Connection(IoService& io, ConnectionId id, SOCKET s, std::string peer);
    ~Connection();

    Connection(const Connection&)            = delete;
    Connection& operator=(const Connection&) = delete;

    ConnectionId       id()   const noexcept { return id_; }
    SOCKET             sock() const noexcept { return socket_; }
    const std::string& peer() const noexcept { return peer_; }

    // 최초 수신을 건다. 완료 포트 등록 직후 1회 호출한다
    bool start();

    // 헤더를 붙여 송신 대기열에 넣는다. 실제 발행은 내부에서 판단한다
    void send(std::uint16_t msg_id, const void* body, std::uint32_t size);

    // 소켓만 닫는다. 발행 중이던 요청은 실패로 완료 통보된다
    void close() noexcept;

    // 아직 소켓으로 나가지 못한 송신분이 있는지 본다. 정상 종료 판정에 쓴다
    bool has_unsent();

    // 요청 속도 제한. 이 연결의 수신 경로에서만 만진다
    void set_rate_limit(int rate_per_sec, int burst) noexcept {
        limiter_.configure(rate_per_sec, burst);
    }
    bool allow_message() noexcept;

    // 아직 내보내지 못한 송신 누적분의 상한(바이트). 0 이면 제한하지 않는다.
    //
    // 상대가 응답을 읽지 않으면 커널 송신 버퍼가 막히고, 그동안 만들어진 응답이
    // 이 버퍼에 계속 쌓인다. 받는 쪽에는 상한이 세 겹 있지만 여기에는 없었다.
    // 기동 시 한 번 정하고 그 뒤로는 바꾸지 않는다
    void set_send_limit(std::size_t bytes) noexcept { send_limit_ = bytes; }

    // 발행을 보내는 쪽이 직접 할지, 완료 포트에 통지만 걸지.
    // 기동 시 한 번 정하고 그 뒤로는 바꾸지 않는다
    void set_direct_send(bool on) noexcept { direct_send_ = on; }

    bool closed()  const noexcept { return closed_.load(std::memory_order_acquire); }
    int  pending() const noexcept { return pending_.load(std::memory_order_acquire); }
    // 해제 가능 조건. 이 둘이 동시에 참일 때만 회수한다
    bool reclaimable() const noexcept { return closed() && pending() == 0; }

    // 수신을 멈춘다 · 다시 건다.
    //
    // 다음 수신을 걸지 않으면 소켓 수신 버퍼가 차고 TCP 창이 줄어든다.
    // 그러면 상대의 송신이 저절로 느려진다 - 거절하지 않고도 속도를 맞추는 방법이다.
    // 로직 스레드에서 호출한다. 실제 재발행은 완료 포트를 거쳐 IOCP 워커가 한다
    void pause_receive() noexcept;
    void resume_receive();
    bool receive_paused() const noexcept {
        return recv_paused_.load(std::memory_order_acquire);
    }

    // 완료 통보 처리. IoService 워커에서만 호출된다
    void on_receive_done(int transferred, bool ok);
    void on_send_done(int transferred, bool ok);
    void on_flush();    // 쌓인 송신분을 실제로 발행한다
    void on_resume();   // 멈춰 두었던 수신을 다시 건다

    void set_message_handler(MessageHandler h) { on_message_ = std::move(h); }
    void set_close_handler(CloseHandler h)     { on_close_   = std::move(h); }

    // 마지막으로 통보를 받은 시각(ms). 하트비트 판정에 쓴다
    std::int64_t last_active_ms() const noexcept {
        return last_active_ms_.load(std::memory_order_relaxed);
    }
    void touch() noexcept;

private:
    bool post_receive();
    void post_send_locked();       // send_mutex_ 를 쥔 상태로 호출한다
    void drain_frames();

    IoService&   io_;
    ConnectionId id_;
    SOCKET       socket_;
    std::string  peer_;

    ReceiveOp    recv_op_;
    ReceiveBuffer recv_buf_;
    RateLimiter   limiter_;

    SendOp     send_op_;
    FlushOp    flush_op_;
    ResumeOp   resume_op_;
    // 발행 통지가 중복으로 쌓이지 않게 한다
    std::atomic<bool> flush_queued_{false};
    // 수신 보류 상태와 재개 통지 중복 방지
    std::atomic<bool> recv_paused_{false};
    std::atomic<bool> resume_queued_{false};
    std::mutex send_mutex_;
    // 대기분을 버퍼 하나에 이어 붙인다. 메시지마다 버퍼를 만들고
    // 발행 직전에 합치면 할당과 복사가 두 번씩 생긴다.
    // 발행할 때는 두 버퍼를 맞바꾸기만 하면 된다
    std::vector<char> send_pending_;       // 아직 발행되지 않은 누적분
    std::vector<char> send_staging_;  // 커널에 넘긴 버퍼
    bool              sending_ = false;
    bool              direct_send_ = false;
    std::size_t       send_limit_ = 0;
    // 누적분에 든 메시지 수. 발행 한 번에 몇 건이 실려 나가는지를 세는 데 쓴다
    std::uint64_t     pending_messages_ = 0;

    std::atomic<int>          pending_{0};
    std::atomic<bool>         closed_{false};
    std::atomic<std::int64_t> last_active_ms_{0};

    MessageHandler on_message_;
    CloseHandler   on_close_;
};

using ConnectionPtr = std::shared_ptr<Connection>;

} // namespace net
