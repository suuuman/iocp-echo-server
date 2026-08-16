#pragma once
//
//  거는 쪽 엔드포인트. Acceptor 와 대칭이다.
//
//  ConnectEx 로 접속을 비동기로 건다.
//  connect() 로 막으면 접속 하나마다 스레드 하나가 묶이고,
//  그러면 클라이언트 스레드 수가 접속 수에 비례한다.
//  측정 도구가 그 구조를 쓰면 무엇을 재는지 알 수 없게 된다.
//
#include <winsock2.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "net/io_operation.h"

namespace net {

class IoService;

class Connector {
public:
    using ConnectedHandler = std::function<void(SOCKET, std::string peer, std::uint64_t id)>;
    using FailedHandler    = std::function<void()>;

    explicit Connector(IoService& io) : io_(io) {}
    ~Connector();

    Connector(const Connector&)            = delete;
    Connector& operator=(const Connector&) = delete;

    void set_handlers(ConnectedHandler on_connected, FailedHandler on_failed);

    // 접속 하나를 건다. 결과는 완료 통보로 돌아온다
    bool connect(const std::string& host, std::uint16_t port);

    void on_connect_done(ConnectOp& op, bool ok);

private:
    ConnectOp* acquire();
    void       release(ConnectOp& op);

    IoService& io_;

    std::mutex                              mutex_;
    std::vector<std::unique_ptr<ConnectOp>> ops_;    // 소유
    std::vector<ConnectOp*>                 free_;   // 재사용 대기

    ConnectedHandler on_connected_;
    FailedHandler    on_failed_;
};

} // namespace net
