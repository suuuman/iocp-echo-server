#pragma once
//
//  채팅 서비스 - 한 요청을 접속자 전원에게 퍼뜨린다.
//
//  접속자 목록을 소유한다. 담당 메시지는 Chat 하나다.
//
//  이 서비스가 기능 축의 값어치를 그대로 보여 준다 -
//  모든 접속자가 한 스레드에 모여 있으므로 브로드캐스트가 목록을 훑는 반복문 하나로 끝난다.
//  연결로 나눴다면 같은 일에 스레드 간 전달이 접속 수만큼 필요하다.
//
//  게임 서비스의 세션 표를 보지 않는다.
//  자기 목록에 conn_id → 연결 참조와 표시 이름만 들고 있으면 되고,
//  송신은 Connection::send 가 자체 잠금을 갖고 있어 어느 스레드가 불러도 안전하다.
//
//  레인은 1 로 고정한다. 나누면 브로드캐스트가 레인 간 전달이 되어
//  이 서비스를 둔 이유가 사라진다.
//
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "app/service.h"
#include "core/latency.h"

namespace app {

class BroadcastService : public Service {
public:
    BroadcastService() : Service("chat") {}
    // 목록이 연결 참조를 붙잡고 있다. 스레드를 멈춘 뒤에 놓아야 한다
    ~BroadcastService() override { stop(); }

protected:
    bool on_created(int count) override;
    void on_event(Lane& lane, InboundEvent& ev) override;
    void on_service(Lane& lane, ServiceMessage& msg) override;
    void on_fault(Lane& lane, net::ConnectionId id) override;
    void on_report(Lane& lane, ServiceStats& out) override;
    void on_after_batch(Lane& lane) override;
    void on_stopped() override;

private:
    struct Member {
        net::ConnectionPtr conn;
        // 게임 서비스가 발급한 세션 키. 도착 전에는 비어 있다
        std::string name;
    };

    void broadcast(const std::string& from, std::string_view text);

    // 레인 1 개짜리 서비스라 표도 하나다. 이 서비스의 스레드만 만진다
    std::unordered_map<net::ConnectionId, Member> members_;

    // 접속 통지보다 세션 키가 먼저 도착한 경우에 잠깐 들고 있는 자리.
    //
    // 두 값이 서로 다른 큐로 들어오기 때문에 생긴다 -
    // 접속 통지는 IOCP 워커가 이 서비스의 수신 큐에 넣고,
    // 세션 키는 게임 서비스가 그 통지를 처리한 뒤 전달 큐에 넣는다.
    // 전달 큐를 먼저 비우므로, 같은 회차에서 세션 키가 앞설 수 있다.
    //
    // 접속 통지가 항상 먼저 적재되므로 늦어도 같은 회차의 수신 큐에 들어 있다.
    // 그래서 쓸모 있는 항목은 그 회차 안에 반드시 옮겨진다.
    //
    // 회차가 끝나고도 남은 것은 주인이 없는 항목이다 - 접속과 종료가 이미 다 지나간 뒤에
    // 세션 키가 도착한 경우다(게임 서비스가 밀리고 접속이 짧으면 그렇게 된다).
    // 지우지 않으면 접속·종료를 되풀이할수록 이 표만 자란다.
    // 그래서 회차 끝에서 비운다
    std::unordered_map<net::ConnectionId, std::string> pending_names_;

    // 주인이 없어 버린 세션 키 수. 0 이 아니면 게임 서비스가 밀리고 있다는 신호다
    std::uint64_t orphan_names = 0;

    std::uint64_t handled         = 0;
    std::uint64_t rejected        = 0;
    std::uint64_t broadcasts      = 0;
    std::uint64_t broadcast_sends = 0;

    // 한 번의 브로드캐스트에 든 시간(µs). 접속 수에 따라 꼬리가 어떻게 자라는지 본다
    core::LatencyHistogram broadcast_dist;
};

} // namespace app
