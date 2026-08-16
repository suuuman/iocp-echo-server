#pragma once
//
//  관리 포트.
//
//  감시 도구와 로드밸런서가 볼 지점을 하나 둔다.
//  주 경로와 완전히 분리한다 - 관리 요청이 게임 처리에 영향을 주면 안 된다.
//
//  완료 포트를 쓰지 않고 스레드 하나로 차단 처리한다.
//  이 포트에 오는 것은 몇 초에 한 번의 수집 요청뿐이라
//  지연보다 "주 경로와 섞이지 않는다" 는 쪽이 중요하다.
//
//  HTTP 로 답한다. 수집 도구 · 로드밸런서 · curl 이 그대로 붙는다.
//
//      GET /health    200 ok  ·  503 unavailable
//      GET /metrics   200 노출 형식 텍스트
//
#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace net {

class AdminServer {
public:
    // 정상 여부. false 면 /health 가 503 으로 답한다
    using HealthFn = std::function<bool()>;
    // 지표 본문을 만든다. 관리 스레드에서 호출된다
    using MetricsFn = std::function<std::string()>;

    AdminServer() = default;
    ~AdminServer();

    AdminServer(const AdminServer&)            = delete;
    AdminServer& operator=(const AdminServer&) = delete;

    // port 가 0 이면 열지 않는다
    bool start(std::uint16_t port, HealthFn health, MetricsFn metrics);
    void stop();

    bool running() const noexcept { return running_.load(std::memory_order_acquire); }

private:
    void run();
    void serve(SOCKET client);

    // 종료가 이 값을 닫아 비우는 동안 관리 스레드는 같은 값을 읽어 accept 에 넘긴다.
    // 두 스레드가 함께 만지므로 원자값으로 둔다.
    // 닫힌 핸들로 accept 하면 오류로 돌아와 루프가 끝나는 것이 종료 방식이다
    std::atomic<SOCKET> listen_socket_{INVALID_SOCKET};
    std::thread         thread_;
    std::atomic<bool>   running_{false};

    HealthFn  health_;
    MetricsFn metrics_;
};

} // namespace net
