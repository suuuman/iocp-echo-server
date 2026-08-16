#include "net/io_service.h"

#include <algorithm>

#include "core/clock.h"
#include "core/log.h"

namespace net {
namespace {
// 워커 종료 신호. 세 값이 모두 0인 통보로 구분한다
constexpr ULONG_PTR kExitKey = 0;
} // namespace

IoService::~IoService() {
    stop();
}

int IoService::decide_worker_count(int requested) {
    if (requested > 0) return requested;

    const unsigned hw = std::thread::hardware_concurrency();
    int n = static_cast<int>(hw == 0 ? 2u : hw / 2);
    // 워커는 대기와 분기만 담당한다. 코어를 모두 쓰면 로직 스레드와 경합한다
    n = std::clamp(n, 2, 4);
    return n;
}

bool IoService::start(const Options& opt) {
    if (running()) return true;
    opt_ = opt;

    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOG_ERROR("WSAStartup failed err=%d", ::WSAGetLastError());
        return false;
    }

    iocp_ = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (iocp_ == nullptr) {
        LOG_ERROR("CreateIoCompletionPort failed err=%lu", ::GetLastError());
        ::WSACleanup();
        return false;
    }

    running_.store(true, std::memory_order_release);

    const int count = decide_worker_count(opt_.worker_count);
    workers_.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
    sweeper_ = std::thread([this] { sweep_loop(); });

    LOG_INFO("io service started workers=%d max_connections=%d", count, opt_.max_connections);
    return true;
}

void IoService::flush_pending(int timeout_ms) {
    const std::int64_t deadline = core::now_ms() + std::max(0, timeout_ms);

    for (;;) {
        std::vector<ConnectionPtr> alive;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            alive.reserve(connections_.size());
            for (auto& [id, conn] : connections_) {
                if (!conn->closed()) alive.push_back(conn);
            }
        }

        // 세는 일은 잠금 밖에서 한다. 연결이 자기 송신 잠금을 잡으므로
        // 두 잠금을 겹쳐 쥐면 순서를 따로 지켜야 한다
        std::size_t waiting = 0;
        for (auto& conn : alive) {
            if (conn->has_unsent()) ++waiting;
        }
        if (waiting == 0) break;

        if (core::now_ms() >= deadline) {
            LOG_WARN("send drain timeout - %zu connections remain", waiting);
            return;
        }
        ::Sleep(5);
    }
    LOG_INFO("send drain done");
}

void IoService::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;

    // 발행 중인 요청을 실패로 완료시켜 회수 경로를 태운다
    {
        std::lock_guard<std::mutex> guard(mutex_);
        for (auto& [id, conn] : connections_) conn->close();
    }

    if (sweeper_.joinable()) sweeper_.join();

    for (std::size_t i = 0; i < workers_.size(); ++i) {
        ::PostQueuedCompletionStatus(iocp_, 0, kExitKey, nullptr);
    }
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();

    {
        std::lock_guard<std::mutex> guard(mutex_);
        connections_.clear();
    }

    if (iocp_) {
        ::CloseHandle(iocp_);
        iocp_ = nullptr;
    }
    ::WSACleanup();
    LOG_INFO("io service stopped");
}

bool IoService::bind_socket(SOCKET s, ConnectionId id) {
    const HANDLE h = ::CreateIoCompletionPort(reinterpret_cast<HANDLE>(s), iocp_,
                                              static_cast<ULONG_PTR>(id), 0);
    if (h == nullptr) {
        LOG_ERROR("bind socket failed err=%lu", ::GetLastError());
        return false;
    }
    return true;
}

bool IoService::post_flush(Connection& conn, FlushOp& op) {
    op.rearm();
    return ::PostQueuedCompletionStatus(iocp_, 0,
                                        static_cast<ULONG_PTR>(conn.id()),
                                        &op) == TRUE;
}

bool IoService::post_resume(Connection& conn, ResumeOp& op) {
    op.rearm();
    return ::PostQueuedCompletionStatus(iocp_, 0,
                                        static_cast<ULONG_PTR>(conn.id()),
                                        &op) == TRUE;
}

ConnectionId IoService::reserve_id() {
    return next_id_.fetch_add(1, std::memory_order_relaxed);
}

ConnectionPtr IoService::adopt(SOCKET s, std::string peer, const SetupHandler& setup,
                               ConnectionId bound_id) {
    const bool         already_bound = (bound_id != 0);
    const ConnectionId id            = already_bound ? bound_id : reserve_id();

    auto conn = std::make_shared<Connection>(*this, id, s, std::move(peer));
    conn->set_rate_limit(opt_.message_rate_limit, opt_.message_burst);
    conn->set_send_limit(opt_.send_buffer_limit > 0
                             ? static_cast<std::size_t>(opt_.send_buffer_limit)
                             : 0);
    conn->set_direct_send(opt_.direct_send);

    // 아래 실패 경로에서 소켓을 따로 닫지 않는다.
    // conn 이 풀리면서 소멸자가 닫는다
    if (!already_bound && !bind_socket(s, id)) {
        return nullptr;
    }

    bool over_limit = false;
    {
        std::lock_guard<std::mutex> guard(mutex_);

        // 판정과 등록을 같은 잠금 안에서 한다.
        // 나눠 두면 워커 여러 개가 동시에 통과해 상한을 넘긴다
        if (opt_.max_connections > 0 &&
            connections_.size() >= static_cast<std::size_t>(opt_.max_connections)) {
            over_limit = true;
        } else {
            connections_.emplace(id, conn);
        }
    }

    if (over_limit) {
        // 상한에 걸린 접속에 응답을 만들어 보내는 것 자체가 비용이다.
        // 자리가 없다는 사실은 연결이 끊기는 것으로 전달된다
        refused_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    // 첫 수신을 걸기 전에 준비를 마친다.
    // 순서가 뒤바뀌면 접속 통지보다 첫 메시지가 먼저 처리된다
    if (setup) setup(*conn);

    // 등록이 끝난 뒤에 첫 수신을 건다.
    // 순서가 뒤바뀌면 완료 통보가 갈 곳이 없다
    if (!conn->start()) {
        std::lock_guard<std::mutex> guard(mutex_);
        connections_.erase(id);
        return nullptr;
    }

    LOG_DEBUG("accepted id=%llu peer=%s",
              static_cast<unsigned long long>(id), conn->peer().c_str());
    return conn;
}

ConnectionPtr IoService::find(ConnectionId id) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto it = connections_.find(id);
    return it == connections_.end() ? nullptr : it->second;
}

std::size_t IoService::connection_count() {
    std::lock_guard<std::mutex> guard(mutex_);
    return connections_.size();
}

void IoService::worker_loop() {
    for (;;) {
        DWORD        transferred = 0;
        ULONG_PTR    key         = 0;
        LPOVERLAPPED ov          = nullptr;

        const BOOL rc = ::GetQueuedCompletionStatus(iocp_, &transferred, &key, &ov, INFINITE);

        if (rc) {
            // 세 값이 모두 0이면 종료 신호다
            if (key == kExitKey && transferred == 0 && ov == nullptr) break;
        } else if (ov == nullptr) {
            // 통보 자체를 받지 못했다. 대기 실패이므로 재시도한다
            const DWORD err = ::GetLastError();
            if (err == WAIT_TIMEOUT) continue;
            if (!running()) break;
            LOG_WARN("completion wait failed err=%lu", err);
            continue;
        }

        // rc == FALSE 이고 ov != nullptr 이면 "요청은 실패했지만 통보는 온" 경우다.
        // 루프를 벗어나지 않고 실패로 표시해 각 경로로 넘긴다.
        // 회수 경로를 하나로 유지하기 위해서다
        const bool ok = (rc == TRUE);
        auto* op = static_cast<IoOperation*>(ov);
        dispatch(static_cast<ConnectionId>(key), op, static_cast<int>(transferred), ok);
    }
}

void IoService::dispatch(ConnectionId id, IoOperation* op, int transferred, bool ok) {
    if (op == nullptr) return;

    if (op->kind == OpKind::Accept) {
        if (accept_handler_) accept_handler_(*static_cast<AcceptOp*>(op), ok);
        return;
    }
    if (op->kind == OpKind::Connect) {
        if (connect_handler_) connect_handler_(*static_cast<ConnectOp*>(op), ok);
        return;
    }

    // 연결이 이미 회수되었을 수 있다. shared_ptr 를 잡아 수명을 확보한 뒤 처리한다
    ConnectionPtr conn = find(id);
    if (!conn) return;

    switch (op->kind) {
        case OpKind::Receive: conn->on_receive_done(transferred, ok); break;
        case OpKind::Send:    conn->on_send_done(transferred, ok);    break;
        case OpKind::Flush:   conn->on_flush();                       break;
        case OpKind::Resume:  conn->on_resume();                      break;
        default: break;
    }
}

void IoService::sweep_loop() {
    const auto interval = std::max(100, opt_.sweep_interval_ms);

    while (running()) {
        ::Sleep(static_cast<DWORD>(interval));

        const std::int64_t now = core::now_ms();
        std::vector<ConnectionPtr> expired;

        {
            std::lock_guard<std::mutex> guard(mutex_);
            for (auto it = connections_.begin(); it != connections_.end();) {
                auto& conn = it->second;

                // 해제 조건은 시간이 아니라 상태다.
                // 소켓이 닫혔고 미완료 작업이 0일 때만 회수한다
                if (conn->reclaimable()) {
                    LOG_DEBUG("reclaim id=%llu",
                              static_cast<unsigned long long>(conn->id()));
                    it = connections_.erase(it);
                    continue;
                }

                if (opt_.heartbeat_timeout_ms > 0 && !conn->closed() &&
                    now - conn->last_active_ms() > opt_.heartbeat_timeout_ms) {
                    expired.push_back(conn);
                }
                ++it;
            }
        }

        // 잠금 밖에서 닫는다. close 가 콜백을 부를 수 있다
        for (auto& conn : expired) {
            LOG_INFO("heartbeat timeout id=%llu peer=%s",
                     static_cast<unsigned long long>(conn->id()), conn->peer().c_str());
            conn->close();
        }
    }
}

} // namespace net
