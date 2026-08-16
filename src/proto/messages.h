#pragma once
//
//  메시지 식별자와 오류 코드. 정의는 docs/protocol.md 와 일치한다.
//
//  응답 ID = 요청 ID + 100 규칙을 둔다.
//  대응 관계를 별도 표로 관리하지 않아도 되고, 누락이 생기지 않는다.
//
#include <cstdint>

namespace proto {

inline constexpr std::uint16_t kAckOffset = 100;

enum MsgId : std::uint16_t {
    // 요청
    kEchoReq    = 1,
    kSaveReq    = 2,
    kHistoryReq = 3,
    kCounterReq = 4,
    kPingReq    = 5,
    kChatReq    = 6,

    // 응답
    kSessionAck = 100,   // 접속 직후 서버가 밀어 준다. 대응 요청이 없다
    kEchoAck    = kEchoReq    + kAckOffset,
    kSaveAck    = kSaveReq    + kAckOffset,
    kHistoryAck = kHistoryReq + kAckOffset,
    kCounterAck = kCounterReq + kAckOffset,
    kPongAck    = kPingReq    + kAckOffset,
    // 보낸 사람에게 답하는 것이 아니라 접속자 전원에게 밀어 준다.
    // 번호 규칙은 그대로 따른다 - 대응 요청이 무엇인지가 번호에 남는다
    kChatPush   = kChatReq    + kAckOffset,

    kErrorAck   = 199,
};

constexpr std::uint16_t ack_of(std::uint16_t req) noexcept {
    return static_cast<std::uint16_t>(req + kAckOffset);
}

// 1~3 은 프레임 오류다. 경계가 어긋났으므로 연결을 끊는다.
// 10 이상은 처리 오류다. 경계는 온전하므로 연결을 유지한다
enum ErrorCode : std::uint16_t {
    kMalformedFrame   = 1,
    kUnknownMessage   = 2,
    kInvalidField     = 3,

    kPayloadTooLong   = 10,
    kLimitOutOfRange  = 11,

    kDbUnavailable    = 20,
    kDbTimeout        = 21,

    kServerBusy       = 30,
};

constexpr bool is_fatal(ErrorCode c) noexcept {
    return c < 10;
}

// 본문 제약
inline constexpr std::uint32_t kMaxPayloadBytes = 512;
inline constexpr std::uint16_t kMinHistoryLimit = 1;
inline constexpr std::uint16_t kMaxHistoryLimit = 100;

} // namespace proto
