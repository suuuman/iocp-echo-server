//
//  부하 · 검증용 클라이언트.
//
//  서버와 같은 네트워크 계층(IoService · Connection · Connector)을 쓴다.
//  연결당 스레드 하나에 차단 소켓을 쓰면 접속 수만큼 스레드가 생기고,
//  그 경합이 측정값에 섞여 서버 성능인지 클라이언트 한계인지 갈라지지 않는다.
//
//  모드
//    load    : 접속을 유지한 채 메시지를 왕복시키고 지연 분포를 낸다
//    churn   : 접속과 종료를 반복한다. 수명 관리 검증용이다
//    idem    : 같은 멱등성 키를 두 번 보낸다. 두 번째가 already_applied 여야 한다
//    verify  : Save 후 History 로 되읽어 순서와 내용이 맞는지 본다
//
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/clock.h"
#include "core/log.h"
#include "core/uuid.h"
#include "net/connector.h"
#include "net/io_service.h"
#include "proto/byte_reader.h"
#include "proto/byte_writer.h"
#include "proto/frame.h"
#include "proto/messages.h"

namespace {

struct Options {
    std::string   host        = "127.0.0.1";
    std::uint16_t port        = 9000;
    int           connections = 1;
    int           seconds     = 10;
    int           payload     = 64;
    int           churn       = 0;
    std::string   mode        = "load";
    std::string   message     = "echo";

    // 채팅 - 브로드캐스트를 재는 데 쓴다.
    // 듣기만 하는 접속과 말하는 접속을 나눈다.
    // 접속 하나가 말하면 접속 전원에게 나가므로, 이 둘의 비가 곧 확산 배수다
    int chat        = 0;    // 채팅 서비스에 붙일 접속 수
    int talkers     = 1;    // 그중 실제로 보내는 접속 수
    int chat_period = 10;   // 말하는 접속 하나의 송신 간격(ms)
};

// 연결 하나의 상태. 핸들러가 이걸 붙잡고 있으므로 찾아볼 필요가 없다
struct BotSession {
    std::string               session_key;
    std::int64_t              sent_us = 0;
    std::vector<std::int64_t> rtt_us;
};

struct Totals {
    std::atomic<std::uint64_t> sent{0};
    std::atomic<std::uint64_t> received{0};
    std::atomic<std::uint64_t> errors{0};
    // 서버가 오류 코드로 되돌린 요청. 연결은 살아 있다
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<int>           connected{0};
    std::atomic<int>           closed{0};
};

// 채팅 집계. 보낸 쪽과 받은 쪽을 따로 센다 -
// 둘의 비가 채팅 1건이 실제 송신 몇 번이 됐는지를 보여 준다
struct ChatTotals {
    std::atomic<std::uint64_t> sent{0};        // 보낸 채팅 수
    std::atomic<std::uint64_t> pushed{0};      // 받은 브로드캐스트 수
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<int>           connected{0};

    // 보낸 사람 이름이 빈 채로 온 브로드캐스트 수.
    // 세션 키는 게임 서비스가 발급해 채팅 서비스로 넘긴다 -
    // 이 값이 0 이 아니면 그 전달이 접속 통지와 어긋난 것이다
    std::atomic<std::uint64_t> unnamed{0};

    std::mutex                lat_mutex;
    std::vector<std::int64_t> latency_us;      // 보낸 시각부터 받은 시각까지
};

// 채팅 본문 앞에 보낸 시각을 16자리 16진수로 박아 둔다.
// 받는 쪽이 그 값으로 지연을 직접 계산한다 - 시계가 같은 장비이므로 보정이 없다
constexpr std::size_t kChatStampChars = 16;

std::string make_chat_text(std::int64_t sent_us, std::size_t payload) {
    char stamp[kChatStampChars + 1];
    std::snprintf(stamp, sizeof(stamp), "%016llx",
                  static_cast<unsigned long long>(sent_us));

    std::string text(stamp, kChatStampChars);
    if (payload > kChatStampChars) text.append(payload - kChatStampChars, 'c');
    return text;
}

// 형식이 어긋나면 0 을 돌려준다. 호출부가 그 표본을 버린다
std::int64_t read_chat_stamp(std::string_view text) {
    if (text.size() < kChatStampChars) return 0;

    std::int64_t v = 0;
    for (std::size_t i = 0; i < kChatStampChars; ++i) {
        const char c = text[i];
        int digit = 0;
        if      (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else return 0;
        v = (v << 4) | digit;
    }
    return v;
}

std::int64_t percentile(std::vector<std::int64_t>& v, double p) {
    if (v.empty()) return 0;
    const auto idx = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(idx), v.end());
    return v[idx];
}

std::uint16_t request_id_of(const std::string& message) {
    if (message == "save")    return proto::kSaveReq;
    if (message == "counter") return proto::kCounterReq;
    if (message == "history") return proto::kHistoryReq;
    return proto::kEchoReq;
}

// ------------------------------------------------------------------
//  차단 소켓 기반 단발 검증 (idem / verify)
//  한 번씩만 주고받으므로 단순한 쪽이 읽기 좋다
// ------------------------------------------------------------------
class SimpleClient {
public:
    ~SimpleClient() { close(); }

    bool connect(const Options& opt) {
        socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == INVALID_SOCKET) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = ::htons(opt.port);
        ::inet_pton(AF_INET, opt.host.c_str(), &addr.sin_addr);

        if (::connect(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            close();
            return false;
        }
        BOOL nodelay = TRUE;
        ::setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
        return true;
    }

    void close() {
        if (socket_ != INVALID_SOCKET) {
            ::closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
    }

    bool send_message(std::uint16_t msg_id, const proto::ByteWriter& w) {
        std::vector<char> frame(static_cast<std::size_t>(proto::kHeaderSize) + w.size());
        proto::write_header(frame.data(), msg_id, w.size());
        if (w.size() > 0) std::memcpy(frame.data() + proto::kHeaderSize, w.data(), w.size());

        const char* p = frame.data();
        int n = static_cast<int>(frame.size());
        while (n > 0) {
            const int sent = ::send(socket_, p, n, 0);
            if (sent <= 0) return false;
            p += sent;
            n -= sent;
        }
        return true;
    }

    bool recv_message(std::uint16_t& msg_id, std::vector<char>& body) {
        for (;;) {
            proto::FrameView view{};
            int consumed = 0;
            const auto r = proto::peek_frame(buf_.data(), static_cast<int>(buf_.size()),
                                             view, consumed);
            if (r == proto::FrameResult::Ok) {
                msg_id = view.msg_id;
                body.assign(view.body, view.body + view.size);
                buf_.erase(buf_.begin(), buf_.begin() + consumed);
                return true;
            }
            if (r == proto::FrameResult::TooLarge) return false;

            char tmp[8192];
            const int n = ::recv(socket_, tmp, sizeof(tmp), 0);
            if (n <= 0) return false;
            buf_.insert(buf_.end(), tmp, tmp + n);
        }
    }

    bool read_session_key() {
        std::uint16_t     id = 0;
        std::vector<char> body;
        if (!recv_message(id, body) || id != proto::kSessionAck) return false;

        proto::ByteReader r(body.data(), static_cast<std::uint32_t>(body.size()));
        session_key_.assign(r.str());
        return r.consumed_all() && session_key_.size() == 36;
    }

    const std::string& session_key() const noexcept { return session_key_; }

private:
    SOCKET            socket_ = INVALID_SOCKET;
    std::vector<char> buf_;
    std::string       session_key_;
};

int run_idem(const Options& opt) {
    SimpleClient c;
    if (!c.connect(opt) || !c.read_session_key()) {
        std::printf("FAIL 접속 또는 세션 키 수신 실패\n");
        return 1;
    }

    const std::string request_key = core::make_session_key();
    std::uint64_t     count[2]{};
    bool              applied[2]{};

    for (int attempt = 0; attempt < 2; ++attempt) {
        proto::ByteWriter w(64);
        w.str(request_key);
        if (!c.send_message(proto::kCounterReq, w)) { std::printf("FAIL 송신\n"); return 1; }

        std::uint16_t     id = 0;
        std::vector<char> body;
        if (!c.recv_message(id, body) || id != proto::kCounterAck) {
            std::printf("FAIL CounterAck 아님 (id=%u)\n", id);
            return 1;
        }
        proto::ByteReader r(body.data(), static_cast<std::uint32_t>(body.size()));
        count[attempt]   = r.u64();
        applied[attempt] = (r.u8() != 0);
        if (!r.consumed_all()) { std::printf("FAIL 본문 형식\n"); return 1; }
    }

    std::printf("1회차 hit_count=%llu already_applied=%d\n",
                static_cast<unsigned long long>(count[0]), applied[0] ? 1 : 0);
    std::printf("2회차 hit_count=%llu already_applied=%d\n",
                static_cast<unsigned long long>(count[1]), applied[1] ? 1 : 0);

    int failures = 0;
    if (applied[0])            { std::printf("FAIL 1회차가 already_applied 로 왔다\n"); ++failures; }
    if (!applied[1])           { std::printf("FAIL 2회차가 already_applied 가 아니다\n"); ++failures; }
    if (count[0] != count[1])  { std::printf("FAIL 재전송으로 값이 늘었다\n"); ++failures; }

    std::printf(failures == 0 ? "PASS 멱등성\n" : "FAIL 멱등성\n");
    return failures == 0 ? 0 : 1;
}

int run_verify(const Options& opt) {
    SimpleClient c;
    if (!c.connect(opt) || !c.read_session_key()) {
        std::printf("FAIL 접속 또는 세션 키 수신 실패\n");
        return 1;
    }

    constexpr int kCount = 5;
    std::vector<std::uint64_t> log_ids;
    int failures = 0;

    for (int i = 0; i < kCount; ++i) {
        proto::ByteWriter w(64);
        w.str("row-" + std::to_string(i));
        if (!c.send_message(proto::kSaveReq, w)) { std::printf("FAIL 송신\n"); return 1; }

        std::uint16_t     id = 0;
        std::vector<char> body;
        if (!c.recv_message(id, body) || id != proto::kSaveAck) {
            std::printf("FAIL SaveAck 아님 (id=%u)\n", id);
            return 1;
        }
        proto::ByteReader r(body.data(), static_cast<std::uint32_t>(body.size()));
        log_ids.push_back(r.u64());
    }

    // 같은 세션 키는 같은 워커로 가므로 순서가 보장되어야 한다
    for (std::size_t i = 1; i < log_ids.size(); ++i) {
        if (log_ids[i] <= log_ids[i - 1]) {
            std::printf("FAIL log_id 가 오름차순이 아니다\n");
            ++failures;
        }
    }

    proto::ByteWriter w(16);
    w.u16(kCount);
    if (!c.send_message(proto::kHistoryReq, w)) { std::printf("FAIL 송신\n"); return 1; }

    std::uint16_t     id = 0;
    std::vector<char> body;
    if (!c.recv_message(id, body) || id != proto::kHistoryAck) {
        std::printf("FAIL HistoryAck 아님 (id=%u)\n", id);
        return 1;
    }

    proto::ByteReader r(body.data(), static_cast<std::uint32_t>(body.size()));
    const auto rows = r.u16();
    std::printf("History 행 수 = %u (기대 %d)\n", rows, kCount);
    if (rows != kCount) ++failures;

    for (int i = 0; i < rows; ++i) {
        const auto row_id = r.u64();
        const auto text   = r.str();
        const auto ms     = r.u64();
        const std::string expect = "row-" + std::to_string(kCount - 1 - i);   // 최신순
        if (text != expect) {
            std::printf("FAIL %d번째 행 내용 불일치\n", i);
            ++failures;
        }
        if (row_id == 0 || ms == 0) { std::printf("FAIL 행 필드가 비었다\n"); ++failures; }
    }
    if (!r.consumed_all()) { std::printf("FAIL 본문에 남은 바이트가 있다\n"); ++failures; }

    std::printf(failures == 0 ? "PASS 저장 · 조회\n" : "FAIL 저장 · 조회\n");
    return failures == 0 ? 0 : 1;
}

// ------------------------------------------------------------------
//  load - 완료 통보 기반. 스레드 수가 접속 수와 무관하다
// ------------------------------------------------------------------
int run_load(const Options& opt) {
    std::printf("load mode : connections=%d seconds=%d payload=%dB message=%s\n",
                opt.connections, opt.seconds, opt.payload, opt.message.c_str());

    net::IoService io;
    net::IoService::Options io_opt{};
    io_opt.worker_count         = 0;
    io_opt.heartbeat_timeout_ms = 0;      // 봇은 만료 판정을 하지 않는다
    io_opt.sweep_interval_ms    = 1000;
    if (!io.start(io_opt)) return 1;

    Totals            totals;
    std::atomic<bool> stop{false};

    const std::uint16_t request_id = request_id_of(opt.message);
    const std::uint16_t expect_id  = proto::ack_of(request_id);
    const std::string   payload(static_cast<std::size_t>(opt.payload), 'x');

    std::mutex                                sessions_mutex;
    std::vector<std::shared_ptr<BotSession>>  sessions;   // 집계용 보관

    // 요청 하나를 보내고 보낸 시각을 남긴다
    auto send_request = [&](net::Connection& conn, BotSession& s) {
        proto::ByteWriter w(static_cast<std::size_t>(opt.payload) + 64);
        if (request_id == proto::kCounterReq) {
            w.str(core::make_session_key());   // 매번 새 멱등성 키
        } else if (request_id == proto::kHistoryReq) {
            w.u16(10);                         // 읽기 전용 - 커밋이 없다
        } else {
            w.str(payload);
        }
        s.sent_us = core::now_us();
        conn.send(request_id, w.data(), w.size());
        totals.sent.fetch_add(1, std::memory_order_relaxed);
    };

    // 채팅 집계는 두 역할이 함께 쓴다.
    // 게임 쪽 접속도 채팅 서비스의 목록에 들어가므로 브로드캐스트를 함께 받는다
    ChatTotals chat;

    std::mutex                      talkers_mutex;
    std::vector<net::ConnectionPtr> talkers;

    // IoService 는 접속 완료 핸들러를 하나만 둔다.
    // 역할이 둘일 때는 한쪽을 다 붙인 뒤 핸들러를 바꿔 단다
    net::Connector connector(io);
    const auto on_failed = [&] { totals.errors.fetch_add(1, std::memory_order_relaxed); };

    auto on_game_connected = [&](SOCKET s, std::string peer, std::uint64_t id) {
            auto state = std::make_shared<BotSession>();
            state->rtt_us.reserve(4096);
            {
                std::lock_guard<std::mutex> guard(sessions_mutex);
                sessions.push_back(state);
            }

            io.adopt(s, std::move(peer), [&, state](net::Connection& conn) {
                conn.set_close_handler([&](net::Connection&) {
                    totals.closed.fetch_add(1, std::memory_order_relaxed);
                });

                // 상태를 붙잡은 핸들러라 조회가 필요 없다.
                // 한 연결의 수신 완료는 직렬화되므로 경합도 없다
                // 봇은 받은 것을 그 자리에서 처리하므로 언제나 참을 돌려준다.
                // 거짓은 "지금 받을 수 없다" 는 뜻이고 그러면 서버가 프레임을 남겨 둔다
                conn.set_message_handler(
                    [&, state](net::Connection& c, const proto::FrameView& frame) {
                        if (frame.msg_id == proto::kSessionAck) {
                            proto::ByteReader r(frame.body, frame.size);
                            state->session_key.assign(r.str());
                            totals.connected.fetch_add(1, std::memory_order_relaxed);
                            if (!stop.load(std::memory_order_relaxed)) send_request(c, *state);
                            return true;
                        }

                        // 오류 응답은 규약대로 나눠 다룬다.
                        // 프레임 오류는 경계가 깨진 것이라 이어 갈 수 없지만,
                        // 처리 오류는 연결이 살아 있으므로 다음 요청을 보낸다.
                        // 이 구분이 없으면 서버가 거절하는 상황을 잴 수 없다
                        if (frame.msg_id == proto::kErrorAck) {
                            proto::ByteReader r(frame.body, frame.size);
                            r.u16();                                    // 원인이 된 요청 ID
                            const auto code = static_cast<proto::ErrorCode>(r.u16());

                            totals.rejected.fetch_add(1, std::memory_order_relaxed);

                            if (proto::is_fatal(code)) {
                                totals.errors.fetch_add(1, std::memory_order_relaxed);
                                c.close();
                                return true;
                            }
                            if (!stop.load(std::memory_order_relaxed)) send_request(c, *state);
                            return true;
                        }

                        // 서버가 밀어 주는 브로드캐스트다. 요청에 대한 답이 아니다.
                        // 접속은 어느 역할이든 채팅 서비스의 목록에도 들어 있으므로
                        // 이쪽으로도 들어온다 - 세기만 하고 넘긴다
                        if (frame.msg_id == proto::kChatPush) {
                            chat.pushed.fetch_add(1, std::memory_order_relaxed);
                            return true;
                        }

                        if (frame.msg_id != expect_id) {
                            totals.errors.fetch_add(1, std::memory_order_relaxed);
                            c.close();
                            return true;
                        }

                        state->rtt_us.push_back(core::now_us() - state->sent_us);
                        totals.received.fetch_add(1, std::memory_order_relaxed);

                        if (!stop.load(std::memory_order_relaxed)) send_request(c, *state);
                        return true;
                    });
            }, id);
    };

    // ---------------------------------------------------------------
    //  채팅 - 듣는 접속과 말하는 접속
    //
    //  말하는 쪽은 정해진 간격으로 보낸다. 받은 만큼 보내는 방식으로 두면
    //  브로드캐스트가 느려질 때 송신도 함께 느려져 부하가 저절로 줄어든다.
    //  간격을 고정해야 서버가 그 부하에서 어떻게 되는지가 나온다
    // ---------------------------------------------------------------
    auto on_chat_connected = [&](SOCKET s, std::string peer, std::uint64_t id) {
            auto lat = std::make_shared<std::vector<std::int64_t>>();
            lat->reserve(4096);

            io.adopt(s, std::move(peer), [&, lat](net::Connection& conn) {
                // 지연 표본은 연결마다 따로 모았다가 종료할 때 한곳에 합친다.
                // 수신 경로에서 공용 배열을 만지면 그 잠금이 측정에 섞인다
                conn.set_close_handler([&, lat](net::Connection&) {
                    totals.closed.fetch_add(1, std::memory_order_relaxed);
                    std::lock_guard<std::mutex> guard(chat.lat_mutex);
                    chat.latency_us.insert(chat.latency_us.end(), lat->begin(), lat->end());
                });

                conn.set_message_handler(
                    [&, lat](net::Connection& c, const proto::FrameView& frame) {
                        if (frame.msg_id == proto::kSessionAck) {
                            const int index = chat.connected.fetch_add(1, std::memory_order_relaxed);
                            // 앞선 접속부터 말하는 쪽으로 삼는다
                            if (index < opt.talkers) {
                                std::lock_guard<std::mutex> guard(talkers_mutex);
                                talkers.push_back(c.shared_from_this());
                            }
                            return true;
                        }

                        if (frame.msg_id == proto::kChatPush) {
                            proto::ByteReader r(frame.body, frame.size);
                            const auto from = r.str();
                            const auto text = r.str();

                            if (from.empty()) {
                                chat.unnamed.fetch_add(1, std::memory_order_relaxed);
                            }

                            const std::int64_t sent_us = read_chat_stamp(text);
                            if (sent_us > 0) lat->push_back(core::now_us() - sent_us);

                            chat.pushed.fetch_add(1, std::memory_order_relaxed);
                            return true;
                        }

                        if (frame.msg_id == proto::kErrorAck) {
                            proto::ByteReader r(frame.body, frame.size);
                            r.u16();
                            const auto code = static_cast<proto::ErrorCode>(r.u16());

                            chat.rejected.fetch_add(1, std::memory_order_relaxed);
                            if (proto::is_fatal(code)) {
                                totals.errors.fetch_add(1, std::memory_order_relaxed);
                                c.close();
                            }
                            return true;
                        }
                        return true;
                    });
            }, id);
    };

    const auto begin_ms = core::now_ms();

    // 채팅 쪽을 먼저 다 붙인다. 남은 완료 통보가 있는 채로 핸들러를 바꾸면
    // 그 접속이 반대쪽 역할로 처리된다
    if (opt.chat > 0) {
        connector.set_handlers(on_chat_connected, on_failed);
        for (int i = 0; i < opt.chat; ++i) {
            if (!connector.connect(opt.host, opt.port)) on_failed();
        }
        for (int i = 0; i < 200; ++i) {
            if (chat.connected.load() >= opt.chat) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    connector.set_handlers(on_game_connected, on_failed);
    for (int i = 0; i < opt.connections; ++i) {
        if (!connector.connect(opt.host, opt.port)) on_failed();
    }

    // 접속이 다 붙을 때까지 잠깐 기다린다. 측정 구간에서 제외한다
    for (int i = 0; i < 200; ++i) {
        if (totals.connected.load() >= opt.connections) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const auto measure_begin_ms = core::now_ms();
    const auto base_recv        = totals.received.load();

    // 말하는 접속이 정해진 간격으로 보낸다.
    // 보내는 일은 스레드 하나로 충분하다 - 브로드캐스트는 서버가 한다
    std::thread talker_thread;
    if (opt.chat > 0 && opt.talkers > 0) {
        talker_thread = std::thread([&] {
            const auto period = std::chrono::milliseconds(std::max(1, opt.chat_period));
            while (!stop.load(std::memory_order_relaxed)) {
                std::vector<net::ConnectionPtr> current;
                {
                    std::lock_guard<std::mutex> guard(talkers_mutex);
                    current = talkers;
                }
                for (auto& conn : current) {
                    if (!conn || conn->closed()) continue;

                    proto::ByteWriter w(static_cast<std::size_t>(opt.payload) + 64);
                    w.str(make_chat_text(core::now_us(),
                                         static_cast<std::size_t>(std::max(opt.payload, 16))));
                    conn->send(proto::kChatReq, w.data(), w.size());
                    chat.sent.fetch_add(1, std::memory_order_relaxed);
                }
                std::this_thread::sleep_for(period);
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::seconds(opt.seconds));
    stop.store(true, std::memory_order_relaxed);
    if (talker_thread.joinable()) talker_thread.join();

    const auto elapsed_ms  = std::max<std::int64_t>(1, core::now_ms() - measure_begin_ms);
    const auto recv        = totals.received.load() - base_recv;
    const auto chat_sent   = chat.sent.load();
    const auto chat_pushed = chat.pushed.load();

    io.stop();

    std::vector<std::int64_t> all;
    {
        std::lock_guard<std::mutex> guard(sessions_mutex);
        for (auto& s : sessions) all.insert(all.end(), s->rtt_us.begin(), s->rtt_us.end());
    }

    std::printf("connected=%d sent=%llu recv=%llu rejected=%llu errors=%llu closed=%d\n",
                totals.connected.load(),
                static_cast<unsigned long long>(totals.sent.load()),
                static_cast<unsigned long long>(totals.received.load()),
                static_cast<unsigned long long>(totals.rejected.load()),
                static_cast<unsigned long long>(totals.errors.load()),
                totals.closed.load());
    std::printf("elapsed=%lldms (접속 대기 %lldms 제외)\n",
                static_cast<long long>(elapsed_ms),
                static_cast<long long>(measure_begin_ms - begin_ms));
    std::printf("throughput = %.0f msg/s\n",
                static_cast<double>(recv) * 1000.0 / static_cast<double>(elapsed_ms));
    std::printf("rtt p50=%lldus p95=%lldus p99=%lldus samples=%zu\n",
                static_cast<long long>(percentile(all, 0.50)),
                static_cast<long long>(percentile(all, 0.95)),
                static_cast<long long>(percentile(all, 0.99)),
                all.size());

    if (opt.chat > 0) {
        std::vector<std::int64_t> chat_lat;
        {
            std::lock_guard<std::mutex> guard(chat.lat_mutex);
            chat_lat.swap(chat.latency_us);
        }

        std::printf("chat members=%d talkers=%d period=%dms sent=%llu pushed=%llu rejected=%llu unnamed=%llu\n",
                    chat.connected.load(), opt.talkers, opt.chat_period,
                    static_cast<unsigned long long>(chat_sent),
                    static_cast<unsigned long long>(chat_pushed),
                    static_cast<unsigned long long>(chat.rejected.load()),
                    static_cast<unsigned long long>(chat.unnamed.load()));
        // 채팅 1건이 실제 송신 몇 번이 됐는지. 접속 수에 붙어 자란다
        std::printf("chat fanout = %.1f sends/message · broadcast = %.0f push/s\n",
                    chat_sent ? static_cast<double>(chat_pushed) /
                                    static_cast<double>(chat_sent)
                              : 0.0,
                    static_cast<double>(chat_pushed) * 1000.0 /
                        static_cast<double>(elapsed_ms));
        std::printf("chat latency p50=%lldus p95=%lldus p99=%lldus samples=%zu\n",
                    static_cast<long long>(percentile(chat_lat, 0.50)),
                    static_cast<long long>(percentile(chat_lat, 0.95)),
                    static_cast<long long>(percentile(chat_lat, 0.99)),
                    chat_lat.size());
    }

    return totals.errors.load() == 0 ? 0 : 1;
}

// ------------------------------------------------------------------
//  chatverify - 브로드캐스트가 실제로 전원에게 가는지 확인한다
//
//  세 접속을 붙이고 하나가 보낸다. 보낸 쪽을 포함해 셋 모두가 받아야 한다 -
//  채팅 서비스가 자기 목록만으로 송신한다는 것이 여기서 드러난다
// ------------------------------------------------------------------
int run_chat_verify(const Options& opt) {
    constexpr int kMembers = 3;

    SimpleClient clients[kMembers];
    std::string  keys[kMembers];

    for (int i = 0; i < kMembers; ++i) {
        if (!clients[i].connect(opt) || !clients[i].read_session_key()) {
            std::printf("FAIL %d번 접속 또는 세션 키 수신 실패\n", i);
            return 1;
        }
        keys[i] = clients[i].session_key();
    }

    const std::string text = "hello-broadcast";

    proto::ByteWriter w(64);
    w.str(text);
    if (!clients[0].send_message(proto::kChatReq, w)) {
        std::printf("FAIL 송신\n");
        return 1;
    }

    int failures = 0;
    for (int i = 0; i < kMembers; ++i) {
        std::uint16_t     id = 0;
        std::vector<char> body;
        if (!clients[i].recv_message(id, body) || id != proto::kChatPush) {
            std::printf("FAIL %d번이 ChatPush 를 받지 못했다 (id=%u)\n", i, id);
            ++failures;
            continue;
        }

        proto::ByteReader r(body.data(), static_cast<std::uint32_t>(body.size()));
        const auto from = r.str();
        const auto got  = r.str();

        if (!r.consumed_all())  { std::printf("FAIL %d번 본문 형식\n", i);      ++failures; }
        if (got != text)        { std::printf("FAIL %d번 내용 불일치\n", i);    ++failures; }
        // 보낸 사람 이름은 게임 서비스가 발급한 세션 키다.
        // 두 서비스가 값을 주고받았다는 것이 이 검사로 확인된다
        if (from != keys[0])    { std::printf("FAIL %d번 보낸 사람 불일치\n", i); ++failures; }
    }

    std::printf(failures == 0 ? "PASS 브로드캐스트\n" : "FAIL 브로드캐스트\n");
    return failures == 0 ? 0 : 1;
}

// ------------------------------------------------------------------
//  churn - 접속과 종료를 반복한다
// ------------------------------------------------------------------
void churn_worker(const Options& opt, Totals& totals, int rounds) {
    for (int i = 0; i < rounds; ++i) {
        SimpleClient c;
        if (!c.connect(opt)) { totals.errors.fetch_add(1); continue; }

        // 절반은 세션 키를 받고 끊고, 절반은 받기 전에 끊는다.
        // 후자가 발행 중인 요청이 남은 상태의 종료를 만든다
        if (i % 2 == 0 && c.read_session_key()) totals.received.fetch_add(1);

        totals.sent.fetch_add(1);
    }
}

int run_churn(const Options& opt) {
    std::printf("churn mode : connections=%d rounds=%d\n", opt.connections, opt.churn);

    Totals totals;
    std::vector<std::thread> threads;
    const auto begin = core::now_ms();
    for (int i = 0; i < opt.connections; ++i) {
        threads.emplace_back([&] { churn_worker(opt, totals, opt.churn); });
    }
    for (auto& t : threads) t.join();

    std::printf("cycles=%llu errors=%llu elapsed=%lldms\n",
                static_cast<unsigned long long>(totals.sent.load()),
                static_cast<unsigned long long>(totals.errors.load()),
                static_cast<long long>(core::now_ms() - begin));
    return totals.errors.load() == 0 ? 0 : 1;
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const auto eq = [&](const char* k) { return std::strcmp(argv[i], k) == 0; };
        const bool has_next = (i + 1 < argc);
        if      (eq("--host")        && has_next) o.host        = argv[++i];
        else if (eq("--port")        && has_next) o.port        = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (eq("--connections") && has_next) o.connections = std::atoi(argv[++i]);
        else if (eq("--seconds")     && has_next) o.seconds     = std::atoi(argv[++i]);
        else if (eq("--payload")     && has_next) o.payload     = std::atoi(argv[++i]);
        else if (eq("--churn")       && has_next) { o.churn = std::atoi(argv[++i]); o.mode = "churn"; }
        else if (eq("--mode")        && has_next) o.mode        = argv[++i];
        else if (eq("--message")     && has_next) o.message     = argv[++i];
        else if (eq("--chat")        && has_next) o.chat        = std::atoi(argv[++i]);
        else if (eq("--talkers")     && has_next) o.talkers     = std::atoi(argv[++i]);
        else if (eq("--chat-period") && has_next) o.chat_period = std::atoi(argv[++i]);
    }
    return o;
}

} // namespace

int main(int argc, char** argv) {
    const Options opt = parse(argc, argv);

    // 봇은 서버와 같은 계층을 쓰므로 그쪽 기록이 함께 나온다.
    // 측정 결과만 보이도록 경고 이상만 남긴다
    core::set_log_level(core::LogLevel::Warn);

    // load 모드는 IoService 가 WSAStartup 을 맡는다.
    // 나머지 모드는 여기서 직접 올린다
    const bool needs_winsock = (opt.mode != "load");
    WSADATA wsa{};
    if (needs_winsock && ::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    int exit_code = 0;
    if      (opt.mode == "idem")       exit_code = run_idem(opt);
    else if (opt.mode == "verify")     exit_code = run_verify(opt);
    else if (opt.mode == "chatverify") exit_code = run_chat_verify(opt);
    else if (opt.mode == "churn")      exit_code = run_churn(opt);
    else                               exit_code = run_load(opt);

    if (needs_winsock) ::WSACleanup();
    return exit_code;
}
