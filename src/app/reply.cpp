#include "app/reply.h"

namespace app {

proto::ByteWriter& scratch() {
    static thread_local proto::ByteWriter w(1024);
    w.clear();
    return w;
}

void send_error(net::Connection& conn, std::uint16_t origin,
                proto::ErrorCode code, const char* detail) {
    auto& w = scratch();
    w.u16(origin);
    w.u16(static_cast<std::uint16_t>(code));
    w.str(detail ? detail : "");
    conn.send(proto::kErrorAck, w.data(), w.size());

    // 프레임 오류는 경계가 이미 어긋난 것이다. 이후 바이트를 신뢰할 수 없다
    if (proto::is_fatal(code)) conn.close();
}

} // namespace app
