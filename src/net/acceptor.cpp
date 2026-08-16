#include "net/acceptor.h"

#include <ws2tcpip.h>

#include "core/log.h"
#include "net/io_service.h"

namespace net {
namespace {
// 청취 소켓의 완료 키. 연결 식별자와 겹치지 않는 값을 쓴다
constexpr ConnectionId kAcceptorKey = static_cast<ConnectionId>(-1);
} // namespace

Acceptor::~Acceptor() {
    stop();
}

bool Acceptor::start(std::uint16_t port, AcceptedHandler on_accepted) {
    on_accepted_ = std::move(on_accepted);

    listen_socket_ = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                                  nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (listen_socket_ == INVALID_SOCKET) {
        LOG_ERROR("WSASocket failed err=%d", ::WSAGetLastError());
        return false;
    }

    // SO_REUSEADDR 이 아니라 SO_EXCLUSIVEADDRUSE 를 쓴다.
    //
    // Windows 의 SO_REUSEADDR 은 POSIX 와 뜻이 다르다 - 이미 그 주소를 쓰고 있는
    // 소켓이 있어도 bind 가 성공한다. 그러면 같은 포트로 인스턴스를 둘 띄웠을 때
    // 두 번째가 실패하지 않고, 들어오는 접속이 둘로 갈린다.
    // 포트를 이미 쓰고 있으면 뜨지 않는 편이 낫다
    BOOL exclusive = TRUE;
    ::setsockopt(listen_socket_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

    sockaddr_in local{};
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port        = ::htons(port);

    if (::bind(listen_socket_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        LOG_ERROR("bind failed port=%u err=%d", port, ::WSAGetLastError());
        stop();
        return false;
    }

    if (::listen(listen_socket_, SOMAXCONN) == SOCKET_ERROR) {
        LOG_ERROR("listen failed err=%d", ::WSAGetLastError());
        stop();
        return false;
    }

    if (!io_.bind_socket(listen_socket_, kAcceptorKey)) {
        stop();
        return false;
    }

    io_.set_accept_handler([this](AcceptOp& op, bool ok) { on_accept_done(op, ok); });

    ops_.reserve(kBacklogOps);
    for (int i = 0; i < kBacklogOps; ++i) {
        ops_.push_back(std::make_unique<AcceptOp>());
        if (!post_accept(*ops_.back())) {
            stop();
            return false;
        }
    }

    LOG_INFO("listening port=%u backlog_ops=%d", port, kBacklogOps);
    return true;
}

void Acceptor::stop() {
    stopping_ = true;

    if (listen_socket_ != INVALID_SOCKET) {
        // 청취 소켓을 닫으면 걸려 있던 수락 요청이 실패로 완료 통보된다
        ::closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
    }

    for (auto& op : ops_) {
        if (op && op->socket != INVALID_SOCKET) {
            ::closesocket(op->socket);
            op->socket = INVALID_SOCKET;
        }
    }
}

bool Acceptor::post_accept(AcceptOp& op) {
    if (stopping_ || listen_socket_ == INVALID_SOCKET) return false;

    // 이전 회차에서 남은 소켓을 정리한다
    if (op.socket != INVALID_SOCKET) {
        ::closesocket(op.socket);
        op.socket = INVALID_SOCKET;
    }

    // 수락될 소켓은 호출자가 미리 만들어 넘긴다
    op.socket = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                             nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (op.socket == INVALID_SOCKET) {
        LOG_ERROR("accept socket create failed err=%d", ::WSAGetLastError());
        return false;
    }

    op.rearm();
    ZeroMemory(op.addr_block, sizeof(op.addr_block));

    DWORD received = 0;
    // 수신 대기 길이를 0으로 둔다.
    // 0이 아니면 첫 데이터가 도착할 때까지 수락이 완료되지 않아
    // 접속만 하고 아무것도 보내지 않는 상대에게 자원이 묶인다
    const BOOL rc = ::AcceptEx(listen_socket_, op.socket,
                               op.addr_block, 0,
                               AcceptOp::kAddrLen, AcceptOp::kAddrLen,
                               &received, &op);
    if (rc == FALSE) {
        const int err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            LOG_ERROR("AcceptEx failed err=%d", err);
            ::closesocket(op.socket);
            op.socket = INVALID_SOCKET;
            return false;
        }
    }
    return true;
}

std::string Acceptor::resolve_peer(const AcceptOp& op) const {
    sockaddr_in* local  = nullptr;
    sockaddr_in* remote = nullptr;
    int local_len  = 0;
    int remote_len = 0;

    ::GetAcceptExSockaddrs(const_cast<char*>(op.addr_block), 0,
                           AcceptOp::kAddrLen, AcceptOp::kAddrLen,
                           reinterpret_cast<sockaddr**>(&local), &local_len,
                           reinterpret_cast<sockaddr**>(&remote), &remote_len);

    char ip[INET_ADDRSTRLEN]{};
    if (remote != nullptr) {
        ::inet_ntop(AF_INET, &remote->sin_addr, ip, sizeof(ip));
        return std::string(ip) + ":" + std::to_string(::ntohs(remote->sin_port));
    }
    return "unknown";
}

void Acceptor::on_accept_done(AcceptOp& op, bool ok) {
    if (!ok) {
        // 종료 중이면 정상 흐름이다
        if (!stopping_) LOG_WARN("accept failed err=%d", ::WSAGetLastError());
        if (op.socket != INVALID_SOCKET) {
            ::closesocket(op.socket);
            op.socket = INVALID_SOCKET;
        }
        post_accept(op);
        return;
    }

    const SOCKET accepted = op.socket;
    op.socket = INVALID_SOCKET;   // 소유권을 넘긴다

    // 청취 소켓의 속성을 승계시킨다.
    // 이걸 빠뜨리면 getpeername 등 일부 호출이 실패한다
    ::setsockopt(accepted, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                 reinterpret_cast<const char*>(&listen_socket_), sizeof(listen_socket_));

    // 작은 응답이 지연되지 않도록 지연 전송을 끈다.
    // 송신 병합은 이 계층에서 직접 하고 있다
    BOOL nodelay = TRUE;
    ::setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

    std::string peer = resolve_peer(op);

    if (on_accepted_) on_accepted_(accepted, std::move(peer));

    // 같은 작업 객체로 곧바로 다시 건다
    post_accept(op);
}

} // namespace net
