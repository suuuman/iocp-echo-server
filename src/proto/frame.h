#pragma once
//
//  프레임 경계 복원.
//
//      +------------------+-------------+------------------+
//      | body_size (u32)  | msg_id(u16) | body (body_size) |
//      +------------------+-------------+------------------+
//
//  body_size 는 본문 길이만 담는다. 헤더 6바이트를 포함하지 않는다.
//  수신부에서 `보유량 >= kHeaderSize + body_size` 한 번으로 완성 여부가 판정된다.
//
#include <cstdint>

#include "proto/byte_order.h"

namespace proto {

inline constexpr int kHeaderSize  = 6;

//  상한을 방향별로 나눈다.
//
//  받는 쪽과 보내는 쪽의 크기가 다르기 때문이다. 하나로 묶으면
//  큰 쪽에 맞춰야 하고, 그러면 받는 쪽 버퍼가 실제 필요보다 커진다.
//  수신 버퍼 상한이 곧 접속당 메모리이므로 그 차이가 그대로 상한 계산에 들어간다.

// 받아들이는 프레임의 본문 상한.
// 이 서버가 받는 요청 중 가장 큰 것은 Echo · Save 의 514바이트다(길이 2 + 본문 512)
inline constexpr std::uint32_t kMaxInboundBody  = 1024;

// 내보내는 프레임의 본문 상한.
// History 응답이 가장 크다 - 100행 × (식별자 8 + 길이 2 + 본문 512 + 시각 8) ≈ 53KB
inline constexpr std::uint32_t kMaxOutboundBody = 64 * 1024;

// 수신 버퍼 내부를 가리키는 참조다. 다음 수신 전에 소비를 마쳐야 한다
struct FrameView {
    std::uint16_t msg_id = 0;
    const char*   body   = nullptr;
    std::uint32_t size   = 0;
};

enum class FrameResult {
    Ok,          // 완성된 프레임 1개를 잘라냈다
    Incomplete,  // 아직 덜 왔다. 다음 수신을 기다린다
    TooLarge,    // body_size 가 받아들이는 상한을 넘었다. 연결을 끊어야 한다
};

// data/size 구간의 선두에서 프레임 하나를 해석한다.
// 성공 시 consumed 에 소비해야 할 바이트 수(헤더 + 본문)를 채운다
inline FrameResult peek_frame(const char* data, int size,
                              FrameView& out, int& consumed) noexcept {
    if (size < kHeaderSize) return FrameResult::Incomplete;

    const std::uint32_t body_size = load_be32(data);
    if (body_size > kMaxInboundBody) return FrameResult::TooLarge;

    const std::uint32_t total = static_cast<std::uint32_t>(kHeaderSize) + body_size;
    if (static_cast<std::uint32_t>(size) < total) return FrameResult::Incomplete;

    out.msg_id = load_be16(data + 4);
    out.body   = data + kHeaderSize;
    out.size   = body_size;
    consumed   = static_cast<int>(total);
    return FrameResult::Ok;
}

// 헤더를 dst 앞 6바이트에 기록한다
inline void write_header(char* dst, std::uint16_t msg_id, std::uint32_t body_size) noexcept {
    store_be32(dst, body_size);
    store_be16(dst + 4, msg_id);
}

} // namespace proto
