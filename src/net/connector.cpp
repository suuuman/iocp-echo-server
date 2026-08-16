#include "net/connector.h"

#include <ws2tcpip.h>

#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "net/io_service.h"

namespace net {
namespace {

// ConnectEx 는 확장 함수라 실행 시점에 주소를 얻어야 한다
LPFN_CONNECTEX lookup_connect_ex(SOCKET s) {
    GUID           guid  = WSAID_CONNECTEX;
    LPFN_CONNECTEX fn    = nullptr;
    DWORD          bytes = 0;

    if (::WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER,
                   &guid, sizeof(guid), &fn, sizeof(fn),
                   &bytes, nullptr, nullptr) == SOCKET_ERROR) {
        return nullptr;
    }
    return fn;
}

} // namespace

Connector::~Connector() {
    std::lock_guard<std::mutex> guard(mutex_);
    for (auto& op : ops_) {
        if (op && op->socket != INVALID_SOCKET) {
            ::closesocket(op->socket);
            op->socket = INVALID_SOCKET;
        }
    }
}

void Connector::set_handlers(ConnectedHandler on_connected, FailedHandler on_failed) {
    on_connected_ = std::move(on_connected);
    on_failed_    = std::move(on_failed);
    io_.set_connect_handler([this](ConnectOp& op, bool ok) { on_connect_done(op, ok); });
}

ConnectOp* Connector::acquire() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!free_.empty()) {
        ConnectOp* op = free_.back();
        free_.pop_back();
        return op;
    }
    ops_.push_back(std::make_unique<ConnectOp>());
    return ops_.back().get();
}

void Connector::release(ConnectOp& op) {
    std::lock_guard<std::mutex> guard(mutex_);
    free_.push_back(&op);
}

bool Connector::connect(const std::string& host, std::uint16_t port) {
    ConnectOp* op = acquire();
    op->rearm();

    op->socket = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                              nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (op->socket == INVALID_SOCKET) {
        release(*op);
        return false;
    }

    // ConnectEx 는 미리 묶인 소켓만 받는다
    sockaddr_in local{};
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port        = 0;
    if (::bind(op->socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        ::closesocket(op->socket);
        op->socket = INVALID_SOCKET;
        release(*op);
        return false;
    }

    // 소켓을 붙이는 시점에 완료 키가 정해진다. 나중에 바꿀 수 없으므로
    // 연결 식별자를 여기서 미리 받아 그대로 쓴다
    op->connection_id = io_.reserve_id();

    if (!io_.bind_socket(op->socket, op->connection_id)) {
        ::closesocket(op->socket);
        op->socket = INVALID_SOCKET;
        release(*op);
        return false;
    }

    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port   = ::htons(port);
    ::inet_pton(AF_INET, host.c_str(), &remote.sin_addr);

    std::snprintf(op->peer, sizeof(op->peer), "%s:%u", host.c_str(), port);

    const LPFN_CONNECTEX connect_ex = lookup_connect_ex(op->socket);
    if (connect_ex == nullptr) {
        LOG_ERROR("ConnectEx lookup failed err=%d", ::WSAGetLastError());
        ::closesocket(op->socket);
        op->socket = INVALID_SOCKET;
        release(*op);
        return false;
    }

    if (connect_ex(op->socket, reinterpret_cast<sockaddr*>(&remote), sizeof(remote),
                   nullptr, 0, nullptr, op) == FALSE) {
        const int err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            LOG_WARN("ConnectEx failed err=%d", err);
            ::closesocket(op->socket);
            op->socket = INVALID_SOCKET;
            release(*op);
            return false;
        }
    }
    return true;
}

void Connector::on_connect_done(ConnectOp& op, bool ok) {
    const SOCKET        s  = op.socket;
    const std::uint64_t id = op.connection_id;
    op.socket        = INVALID_SOCKET;
    op.connection_id = 0;

    std::string peer = op.peer;
    release(op);

    if (!ok || s == INVALID_SOCKET) {
        if (s != INVALID_SOCKET) ::closesocket(s);
        if (on_failed_) on_failed_();
        return;
    }

    // 소켓이 접속 상태를 반영하도록 갱신한다.
    // 이걸 빠뜨리면 getpeername 등 일부 호출이 실패한다
    ::setsockopt(s, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);

    BOOL nodelay = TRUE;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

    if (on_connected_) on_connected_(s, std::move(peer), id);
}

} // namespace net
