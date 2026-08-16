#pragma once
//
//  IOCP 워커가 로직 스레드로 넘기는 항목.
//
//  접속 · 종료 · 메시지를 한 큐에 태그로 묶는다.
//  큐를 나누면 "접속보다 첫 메시지가 먼저 처리되는" 순서 역전이 생긴다.
//
//  본문을 인라인 배열로 둔 이유 -
//    ① proto::FrameView::body 는 수신 버퍼 내부를 가리킨다. 소비 위치가 전진하고
//       다음 수신이 그 자리를 덮으므로 반드시 복사해야 한다
//    ② 요청 본문의 상한이 정해져 있다(payload 512바이트 + 길이 2바이트).
//       따라서 항목마다 힙 할당을 할 이유가 없다
//
#include <array>
#include <cstdint>

#include "net/connection.h"

namespace app {

// 요청 본문의 상한. proto::kMaxPayloadBytes 를 담고도 남는 크기다
inline constexpr int kMaxEventBody = 640;

enum class EventKind : std::uint8_t {
    Connected,
    Disconnected,
    Message,
};

struct InboundEvent {
    EventKind         kind    = EventKind::Message;
    net::ConnectionId conn_id = 0;
    std::uint16_t     msg_id  = 0;
    std::uint16_t     size    = 0;

    // 본문이 상한을 넘어 싣지 못한 경우. 로직 스레드가 오류로 응답한다
    bool oversized = false;

    // 적재 시각. 왕복 지연에서 큐 대기 시간을 분리하는 데 쓴다
    std::int64_t enqueued_us = 0;

    // Connected 일 때만 채운다. 세션이 살아 있는 동안 연결을 붙잡아 둔다
    net::ConnectionPtr conn;

    // 초기화하지 않는다. 앞의 size 바이트만 읽으므로 값을 채울 이유가 없고,
    // 항목마다 640바이트를 0으로 미는 비용이 그대로 처리량에 들어간다
    std::array<char, kMaxEventBody> body;
};

} // namespace app
