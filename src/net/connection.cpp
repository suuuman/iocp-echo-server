#include "net/connection.h"

#include <cstring>

#include "core/clock.h"
#include "core/log.h"
#include "net/io_service.h"

namespace net {

Connection::Connection(IoService& io, ConnectionId id, SOCKET s, std::string peer)
    : io_(io), id_(id), socket_(s), peer_(std::move(peer)) {
    last_active_ms_.store(core::now_ms(), std::memory_order_relaxed);
}

Connection::~Connection() {
    // 여기 도달했다는 것은 미완료 작업이 0이라는 뜻이다
    if (socket_ != INVALID_SOCKET) {
        ::closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}

bool Connection::allow_message() noexcept {
    // 제한이 꺼져 있으면 시계도 보지 않는다.
    // 이 판정은 메시지마다 지나가므로 꺼진 경로가 공짜여야 한다
    if (!limiter_.enabled()) return true;
    return limiter_.allow(core::now_ms());
}

bool Connection::has_unsent() {
    std::lock_guard<std::mutex> guard(send_mutex_);
    return sending_ || !send_pending_.empty();
}

void Connection::touch() noexcept {
    last_active_ms_.store(core::now_ms(), std::memory_order_relaxed);
}

bool Connection::start() {
    return post_receive();
}

void Connection::close() noexcept {
    // 두 번 닫히지 않게 한 번만 통과시킨다
    bool expected = false;
    if (!closed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    if (socket_ != INVALID_SOCKET) {
        // 지연 종료를 끄고 즉시 끊는다. 대기 상태 소켓이 쌓이는 것을 피한다.
        // 발행 중이던 요청은 실패로 완료 통보되므로 회수 경로는 그대로 유지된다
        LINGER opt{ 1, 0 };
        ::setsockopt(socket_, SOL_SOCKET, SO_LINGER,
                     reinterpret_cast<const char*>(&opt), sizeof(opt));
        ::shutdown(socket_, SD_BOTH);
        ::closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

    if (on_close_) on_close_(*this);
}

// -------------------------------------------------------------------
//  수신
// -------------------------------------------------------------------
bool Connection::post_receive() {
    if (closed()) return false;

    if (!recv_buf_.reserve_free()) {
        // 상한 초과. 단일 메시지가 버퍼 상한을 넘는 경우다
        LOG_WARN("recv buffer limit exceeded id=%llu peer=%s",
                 static_cast<unsigned long long>(id_), peer_.c_str());
        close();
        return false;
    }

    WSABUF buf{};
    buf.buf = recv_buf_.writable();
    buf.len = static_cast<ULONG>(recv_buf_.writable_size());

    recv_op_.rearm();

    DWORD flags = 0;
    DWORD bytes = 0;

    pending_.fetch_add(1, std::memory_order_acq_rel);

    const int rc = ::WSARecv(socket_, &buf, 1, &bytes, &flags, &recv_op_, nullptr);
    if (rc == SOCKET_ERROR) {
        const int err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            // 접수되지 않았다. 완료 통보가 오지 않으므로 여기서 되돌린다
            pending_.fetch_sub(1, std::memory_order_acq_rel);
            close();
            return false;
        }
    }
    return true;
}

void Connection::on_receive_done(int transferred, bool ok) {
    if (!ok || transferred <= 0) {
        // 전송량 0은 상대의 정상 종료다
        close();
        pending_.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    touch();
    recv_buf_.commit_write(transferred);

    drain_frames();

    // 소비 쪽이 밀려 멈춰 둔 상태면 다음 수신을 걸지 않는다.
    // 여기서 걸지 않는 것이 곧 상대에게 보내는 감속 신호다
    if (!closed() && !receive_paused()) post_receive();

    pending_.fetch_sub(1, std::memory_order_acq_rel);
}

void Connection::pause_receive() noexcept {
    recv_paused_.store(true, std::memory_order_release);
}

void Connection::resume_receive() {
    if (!recv_paused_.load(std::memory_order_acquire)) return;

    // 통지가 겹쳐 쌓이지 않게 한 번만 밀어 넣는다
    if (resume_queued_.exchange(true, std::memory_order_acq_rel)) return;

    pending_.fetch_add(1, std::memory_order_acq_rel);
    if (!io_.post_resume(*this, resume_op_)) {
        pending_.fetch_sub(1, std::memory_order_acq_rel);
        resume_queued_.store(false, std::memory_order_release);
    }
}

void Connection::on_resume() {
    recv_paused_.store(false, std::memory_order_release);
    resume_queued_.store(false, std::memory_order_release);

    // 멈추기 전에 남겨 둔 프레임부터 소비한다.
    // 여기서 다시 가득 차면 그 안에서 또 멈춘다
    drain_frames();

    // 멈춰 있는 동안 걸린 수신이 없다. 여기서 다시 시작한다
    if (!closed() && !receive_paused()) post_receive();

    pending_.fetch_sub(1, std::memory_order_acq_rel);
}

void Connection::drain_frames() {
    // 한 번의 수신에 메시지가 여러 개 들어올 수 있다
    for (;;) {
        proto::FrameView view{};
        int consumed = 0;

        const auto r = proto::peek_frame(recv_buf_.readable(),
                                         recv_buf_.readable_size(),
                                         view, consumed);
        if (r == proto::FrameResult::Incomplete) break;

        if (r == proto::FrameResult::TooLarge) {
            // 경계가 이미 어긋났다. 이후 바이트를 신뢰할 수 없으므로 끊는다
            LOG_WARN("malformed frame id=%llu peer=%s",
                     static_cast<unsigned long long>(id_), peer_.c_str());
            close();
            return;
        }

        if (on_message_ && !on_message_(*this, view)) {
            // 소비 쪽이 받지 못했다. 이 프레임을 버리지 않고 버퍼에 남긴다.
            // 다시 걸릴 때 같은 자리에서 이어 읽는다
            return;
        }

        // 핸들러가 body 참조를 끝낸 뒤에 소비 위치를 옮긴다
        recv_buf_.consume(consumed);

        if (closed()) return;
    }
}

// -------------------------------------------------------------------
//  송신
// -------------------------------------------------------------------
void Connection::send(std::uint16_t msg_id, const void* body, std::uint32_t size) {
    if (closed()) return;

    if (size > proto::kMaxOutboundBody) {
        // 보낼 수 없는 응답이다. 조용히 버리면 기다리는 쪽이 영원히 기다린다
        LOG_ERROR("send body too large id=%llu size=%u",
                  static_cast<unsigned long long>(id_), size);
        close();
        return;
    }

    const std::size_t adding = static_cast<std::size_t>(proto::kHeaderSize) + size;
    bool overflow = false;

    {
        std::lock_guard<std::mutex> guard(send_mutex_);

        // 발행 중인 분량까지 함께 센다. 그쪽도 아직 나가지 않은 것이다
        const std::size_t queued = send_pending_.size() + send_staging_.size();

        if (send_limit_ > 0 && queued + adding > send_limit_) {
            overflow = true;
        } else {
            // 누적 버퍼 뒤에 헤더와 본문을 그대로 이어 붙인다
            const std::size_t offset = send_pending_.size();
            send_pending_.resize(offset + adding);

            proto::write_header(send_pending_.data() + offset, msg_id, size);
            if (size > 0) {
                std::memcpy(send_pending_.data() + offset + proto::kHeaderSize, body, size);
            }
            ++pending_messages_;

            if (direct_send_) {
                // 이 자리에서 발행한다. 발행 중이면 아무것도 하지 않고,
                // 지금 쌓인 것은 완료 통보가 온 뒤 한 번에 나간다
                post_send_locked();
            } else if (!sending_ && !flush_queued_.exchange(true, std::memory_order_acq_rel)) {
                // 발행을 워커에게 넘긴다. 보내는 쪽 스레드에 송신 비용이 실리지 않는다.
                //
                // 통지 자체도 완료 포트에 거는 시스템 호출이다.
                // 발행 수와 나란히 세어야 통지 하나에 몇 건이 실려 나갔는지가 나온다 -
                // 그 값이 1 이면 이 경로가 시스템 호출을 하나 더 쓰고 있는 것이다
                io_.count_flush_post();
                pending_.fetch_add(1, std::memory_order_acq_rel);
                if (!io_.post_flush(*this, flush_op_)) {
                    // 통지에 실패하면 통보가 오지 않는다. 되돌리고 직접 발행한다
                    pending_.fetch_sub(1, std::memory_order_acq_rel);
                    flush_queued_.store(false, std::memory_order_release);
                    post_send_locked();
                }
            }
        }
    }

    if (overflow) {
        // 오류로 답하지 않는다. 그 응답도 같은 버퍼에 쌓이므로
        // 늘어나는 것을 늘려서 막는 꼴이 된다.
        // 잠금을 놓고 끊는다 - close 는 콜백을 부른다
        io_.count_send_overflow();
        LOG_WARN("send buffer limit exceeded id=%llu peer=%s limit=%zu",
                 static_cast<unsigned long long>(id_), peer_.c_str(), send_limit_);
        close();
    }
}

void Connection::on_flush() {
    {
        std::lock_guard<std::mutex> guard(send_mutex_);
        flush_queued_.store(false, std::memory_order_release);
        post_send_locked();
    }
    pending_.fetch_sub(1, std::memory_order_acq_rel);
}

void Connection::post_send_locked() {
    // 발행 중이면 아무것도 하지 않는다. 지금 쌓인 것들은 다음 한 번에 나간다
    if (sending_ || closed() || send_pending_.empty()) return;

    // 맞바꾸기만 한다. 복사가 없고 두 버퍼의 용량이 그대로 재사용된다
    send_staging_.swap(send_pending_);
    send_pending_.clear();

    io_.count_send_issued(pending_messages_);
    pending_messages_ = 0;

    WSABUF buf{};
    buf.buf = send_staging_.data();
    buf.len = static_cast<ULONG>(send_staging_.size());

    send_op_.rearm();
    sending_ = true;

    DWORD bytes = 0;
    pending_.fetch_add(1, std::memory_order_acq_rel);

    const int rc = ::WSASend(socket_, &buf, 1, &bytes, 0, &send_op_, nullptr);
    if (rc == SOCKET_ERROR) {
        const int err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            pending_.fetch_sub(1, std::memory_order_acq_rel);
            sending_ = false;
            close();
        }
    }
}

void Connection::on_send_done(int transferred, bool ok) {
    bool fail = !ok;

    {
        std::lock_guard<std::mutex> guard(send_mutex_);

        const int staged = static_cast<int>(send_staging_.size());

        // 중첩 송신은 대체로 전량을 보내고 완료된다. 그 성질에 기대어
        // 부분 전송을 오류로 처리하면 보내던 데이터를 잃는다.
        // 보낸 만큼만 잘라 내고 나머지를 되돌린다
        if (!fail && transferred < staged) {
            if (transferred <= 0) {
                // 한 바이트도 나가지 못했다. 되돌려도 같은 자리에서 멈춘다
                LOG_WARN("send made no progress id=%llu 0/%d",
                         static_cast<unsigned long long>(id_), staged);
                fail = true;
            } else {
                io_.count_partial_send();
                LOG_WARN("partial send id=%llu %d/%d - 남은 구간을 다시 싣는다",
                         static_cast<unsigned long long>(id_), transferred, staged);

                requeue_unsent(send_pending_, send_staging_,
                               static_cast<std::size_t>(transferred));
            }
        }

        sending_ = false;
        send_staging_.clear();

        if (!fail && !closed()) {
            touch();
            post_send_locked();   // 되돌린 구간과 발행 중에 쌓인 것을 이어서 내보낸다
        }
    }

    if (fail) close();

    pending_.fetch_sub(1, std::memory_order_acq_rel);
}

} // namespace net
