#include "net/admin_server.h"

#include <ws2tcpip.h>

#include <cstdio>

#include "core/log.h"

namespace net {
namespace {

// 요청 줄만 읽으면 되므로 크게 잡을 이유가 없다
constexpr int kRequestMax = 2048;

// 느린 상대가 관리 스레드를 붙잡지 못하게 한다
constexpr int kIoTimeoutMs = 3000;

std::string http_response(const char* status, const std::string& body) {
    char header[256];
    const int n = std::snprintf(header, sizeof(header),
                                "HTTP/1.1 %s\r\n"
                                "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
                                "Content-Length: %zu\r\n"
                                "Connection: close\r\n"
                                "\r\n",
                                status, body.size());
    if (n <= 0) return {};
    return std::string(header, static_cast<std::size_t>(n)) + body;
}

// "GET /metrics HTTP/1.1" 에서 경로만 떼어 낸다
std::string request_path(const std::string& request) {
    const auto first = request.find(' ');
    if (first == std::string::npos) return {};

    const auto second = request.find(' ', first + 1);
    if (second == std::string::npos) return {};

    return request.substr(first + 1, second - first - 1);
}

void send_all(SOCKET s, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const int n = ::send(s, data.data() + sent,
                             static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return;
        sent += static_cast<std::size_t>(n);
    }
}

} // namespace

AdminServer::~AdminServer() {
    stop();
}

bool AdminServer::start(std::uint16_t port, HealthFn health, MetricsFn metrics) {
    if (port == 0) return true;      // 열지 않는다
    if (running()) return true;

    health_  = std::move(health);
    metrics_ = std::move(metrics);

    const SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        LOG_ERROR("admin socket failed err=%d", ::WSAGetLastError());
        return false;
    }

    // SO_REUSEADDR 이 아니라 SO_EXCLUSIVEADDRUSE 를 쓴다.
    //
    // Windows 의 SO_REUSEADDR 은 POSIX 와 뜻이 다르다 - 이미 그 주소를 쓰고 있는
    // 소켓이 있어도 bind 가 성공한다. 그러면 인스턴스를 둘 띄웠을 때
    // 두 번째가 실패하지 않고 접속을 나눠 가지며, 어느 쪽이 받을지도 정해지지 않는다.
    // 관리 포트에서 그 일이 나면 지표를 누구 것인지 모른 채 읽게 된다
    BOOL exclusive = TRUE;
    ::setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

    sockaddr_in local{};
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port        = ::htons(port);

    if (::bind(listener, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR ||
        ::listen(listener, 8) == SOCKET_ERROR) {
        LOG_ERROR("admin listen failed port=%u err=%d", port, ::WSAGetLastError());
        ::closesocket(listener);
        return false;
    }

    // 스레드를 만들기 전에 넣는다. 만든 뒤에 넣으면 그 사이 accept 가 빈 값을 본다
    listen_socket_.store(listener, std::memory_order_release);

    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { run(); });

    LOG_INFO("admin port=%u ready - /health /metrics", port);
    return true;
}

void AdminServer::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;

    // 청취 소켓을 닫으면 기다리던 accept 가 실패로 돌아온다.
    // 꺼내면서 비우므로 관리 스레드가 이미 닫힌 핸들을 다시 닫는 일이 없다
    const SOCKET listener = listen_socket_.exchange(INVALID_SOCKET, std::memory_order_acq_rel);
    if (listener != INVALID_SOCKET) ::closesocket(listener);

    if (thread_.joinable()) thread_.join();

    LOG_INFO("admin stopped");
}

void AdminServer::run() {
    while (running()) {
        const SOCKET listener = listen_socket_.load(std::memory_order_acquire);
        if (listener == INVALID_SOCKET) break;   // 종료가 이미 닫았다

        const SOCKET client = ::accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (!running()) break;
            continue;
        }

        DWORD timeout = kIoTimeoutMs;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char*>(&timeout), sizeof(timeout));

        serve(client);
        ::closesocket(client);
    }
}

void AdminServer::serve(SOCKET client) {
    // 요청 줄이 한 번에 다 온다는 보장이 없다.
    // 스트림이므로 어디서 끊겨 오든 이상하지 않고, 한 번만 읽으면
    // 두 조각으로 나뉘어 온 요청에서 경로를 찾지 못한다.
    //
    // 머리부 끝(\r\n\r\n)까지 읽는다. 상한과 시간 제한이 걸려 있으므로
    // 끝을 보내지 않는 상대가 이 스레드를 붙잡지는 못한다
    std::string request;
    request.reserve(256);

    for (;;) {
        char chunk[512];
        const int received = ::recv(client, chunk, sizeof(chunk), 0);
        if (received <= 0) {
            // 끊겼거나 시간이 지났다. 그때까지 받은 것으로 판단한다 -
            // 머리부 끝을 보내지 않는 상대도 요청 줄은 보냈을 수 있다
            break;
        }

        request.append(chunk, static_cast<std::size_t>(received));

        if (request.find("\r\n\r\n") != std::string::npos) break;
        // 줄바꿈 하나만 보내고 마는 상대도 있다. 요청 줄이 끝났으면 그것으로 족하다
        if (request.find('\n') != std::string::npos) break;

        if (request.size() >= static_cast<std::size_t>(kRequestMax)) {
            // 머리부만 받는데 이 크기를 넘었다. 정상 요청이 아니다
            send_all(client, http_response("431 Request Header Fields Too Large", "too large\n"));
            return;
        }
    }

    if (request.empty()) return;

    const std::string path = request_path(request);

    if (path == "/health") {
        const bool ok = health_ ? health_() : true;
        send_all(client, ok ? http_response("200 OK", "ok\n")
                            : http_response("503 Service Unavailable", "unavailable\n"));
        return;
    }

    if (path == "/metrics") {
        const std::string body = metrics_ ? metrics_() : std::string{};
        send_all(client, http_response("200 OK", body));
        return;
    }

    send_all(client, http_response("404 Not Found", "not found\n"));
}

} // namespace net
