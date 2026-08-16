#pragma once
//
//  청취 소켓과 수락 작업을 보유한다.
//
//  수락 요청을 미리 여러 건 걸어 둔다.
//  접속이 몰릴 때 요청을 그때부터 만들면 그 사이 도착분이 대기열에 쌓이기 때문이다.
//  하나가 완료되면 같은 작업 객체를 재사용해 곧바로 다시 건다.
//
#include <winsock2.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "net/io_operation.h"

namespace net {

class IoService;

class Acceptor {
public:
    // 미리 걸어 두는 수락 요청 수
    static constexpr int kBacklogOps = 64;

    using AcceptedHandler = std::function<void(SOCKET, std::string peer)>;

    explicit Acceptor(IoService& io) : io_(io) {}
    ~Acceptor();

    Acceptor(const Acceptor&)            = delete;
    Acceptor& operator=(const Acceptor&) = delete;

    bool start(std::uint16_t port, AcceptedHandler on_accepted);
    void stop();

    // 수락 완료 통보. IoService 가 넘긴다
    void on_accept_done(AcceptOp& op, bool ok);

private:
    bool post_accept(AcceptOp& op);
    std::string resolve_peer(const AcceptOp& op) const;

    IoService& io_;
    SOCKET     listen_socket_ = INVALID_SOCKET;
    bool       stopping_      = false;

    std::vector<std::unique_ptr<AcceptOp>> ops_;
    AcceptedHandler on_accepted_;
};

} // namespace net
