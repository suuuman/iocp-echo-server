#pragma once
//
//  응답 조립과 오류 응답.
//
//  서비스마다 따로 두면 같은 코드가 갈라진다. 오류 형식은 프로토콜의 일부이고
//  어느 서비스가 답하든 같아야 하므로 한 자리에 둔다.
//
#include <cstdint>

#include "net/connection.h"
#include "proto/byte_writer.h"
#include "proto/messages.h"

namespace app {

// 응답 조립용. 스레드마다 하나씩 두어 경합이 없고,
// 용량이 유지되므로 첫 회차 이후로는 할당이 생기지 않는다.
// 돌려받은 뒤 다음 호출 전까지만 쓴다
proto::ByteWriter& scratch();

// 오류 응답. 프레임 오류(코드 10 미만)면 연결도 끊는다
void send_error(net::Connection& conn, std::uint16_t origin,
                proto::ErrorCode code, const char* detail);

} // namespace app
