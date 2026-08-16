#pragma once
//
//  중첩 입출력 작업 객체.
//
//  커널은 요청을 접수한 뒤 "나중에" 이 구조체와 버퍼에 접근한다.
//  따라서 완료 통보가 돌아오기 전에는 어떤 경우에도 해제되면 안 된다.
//  수명 규칙은 Connection 이 보유한 미완료 카운터가 담당한다.
//
#include <winsock2.h>
#include <mswsock.h>

#include <cstdint>

namespace net {

enum class OpKind : std::uint8_t {
    Accept,
    Receive,
    Send,
    // 실제 입출력이 아니라 "보낼 것이 쌓였다" 는 통지다.
    // 로직 스레드가 시스템 호출을 직접 하지 않게 하려고 둔다
    Flush,
    // "멈춰 둔 수신을 다시 걸어라" 는 통지다. Flush 와 같은 이유로 둔다
    Resume,
    // 거는 쪽. 봇이 쓴다
    Connect,
};

// OVERLAPPED 를 첫 멤버로 두고 상속한다.
// 완료 통보는 OVERLAPPED* 로 돌아오므로 여기서 원래 타입을 되찾는다
struct IoOperation : OVERLAPPED {
    OpKind kind;

    explicit IoOperation(OpKind k) noexcept : OVERLAPPED{}, kind(k) {}

    // 재사용 전 반드시 호출한다. 커널이 남긴 이전 결과를 지운다
    void rearm() noexcept {
        Internal = 0; InternalHigh = 0;
        Offset = 0; OffsetHigh = 0;
        hEvent = nullptr;
    }
};

struct AcceptOp : IoOperation {
    // AcceptEx 는 로컬 · 원격 주소를 각각 sockaddr 크기 + 16 바이트로 기록한다
    static constexpr int kAddrLen = sizeof(sockaddr_in) + 16;

    SOCKET socket = INVALID_SOCKET;
    char   addr_block[kAddrLen * 2]{};

    AcceptOp() noexcept : IoOperation(OpKind::Accept) {}
};

struct ReceiveOp : IoOperation {
    ReceiveOp() noexcept : IoOperation(OpKind::Receive) {}
};

struct SendOp : IoOperation {
    SendOp() noexcept : IoOperation(OpKind::Send) {}
};

// 완료 포트에 직접 밀어 넣는다. 커널 입출력을 걸지 않는다
struct FlushOp : IoOperation {
    FlushOp() noexcept : IoOperation(OpKind::Flush) {}
};

struct ResumeOp : IoOperation {
    ResumeOp() noexcept : IoOperation(OpKind::Resume) {}
};

struct ConnectOp : IoOperation {
    SOCKET socket = INVALID_SOCKET;
    // 완료 키로 쓸 식별자. 소켓을 붙일 때 정해지므로 접속 전에 발급받는다
    std::uint64_t connection_id = 0;
    char          peer[64]{};

    ConnectOp() noexcept : IoOperation(OpKind::Connect) {}
};

} // namespace net
