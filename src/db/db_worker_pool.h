#pragma once
//
//  DB 워커 풀.
//
//  워커마다 커넥션 하나와 큐 하나를 둔다.
//  세션 키의 해시로 워커를 고르므로 같은 키의 요청은 항상 같은 워커로 간다.
//  그래서 순서가 보장되고, 처리 중인 키를 따로 관리할 필요가 없다.
//
//  비용 - 부하가 특정 키에 몰리면 그 워커만 밀린다. 동적 분산은 하지 않는다.
//
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/swap_queue.h"
#include "db/db_types.h"
#include "db/mysql_connection.h"
#include "db/request_dispatcher.h"

namespace db {

// 요청을 워커에 배정하는 방식.
// 어느 쪽이든 같은 세션 키의 순서는 보장된다. 다른 것은 그 방법이다
enum class Dispatch : std::uint8_t {
    // hash(session_key) % worker_count 로 워커를 고정한다.
    // 배정에 잠금도 상태도 없다. 대신 분산이 정적이다
    Static,
    // 처리 중인 키를 피해 한가한 워커가 집는다.
    // 부하가 키마다 달라도 워커가 놀지 않는다. 대신 배정마다 잠금을 잡는다
    Dynamic,
};

class WorkerPool {
public:
    using ResponseSink = std::function<void(std::unique_ptr<Response>)>;

    WorkerPool() = default;
    ~WorkerPool();

    WorkerPool(const WorkerPool&)            = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    // 연결에 실패해도 기동은 성공시킨다.
    // DB 가 늦게 뜨는 경우에 서버가 못 뜨면 운영에서 곤란하다
    // batch_size 는 한 트랜잭션에 묶을 `Save` 의 최대 건수다.
    // 1 이면 묶지 않는다 - 요청마다 커밋이 한 번씩 일어난다
    bool start(const ConnectionConfig& cfg, int worker_count, ResponseSink sink,
               Dispatch dispatch = Dispatch::Static, int batch_size = 1);
    void stop();

    // 설정 문자열을 배정 방식으로 옮긴다. 모르는 값이면 Static
    static Dispatch parse_dispatch(const std::string& name);
    static const char* dispatch_name(Dispatch d);

    // 새 요청만 막고 큐에 남은 것은 기한까지 정상 처리한다.
    // 종료 신호를 바로 주면 대기 중이던 요청이 전부 Unavailable 응답이 된다
    void drain(int timeout_ms);

    // 로직 스레드에서 호출한다. 큐가 가득 차면 false
    bool submit(std::unique_ptr<Request> req);

    int         worker_count() const noexcept { return static_cast<int>(workers_.size()); }
    // 워커가 하나도 없으면 DB 를 쓰지 않는 구성이다.
    // 요청을 큐가 가득 찼다고 거절하면 원인을 잘못 알린다
    bool        enabled() const noexcept { return !workers_.empty(); }
    std::size_t queued() const;
    int         connected_workers() const;

    // 큐 상한. 넘으면 요청을 거절한다. 무한히 쌓느니 거절하는 편이 낫다
    static constexpr std::size_t kMaxQueuePerWorker = 4096;

private:
    struct Worker {
        MysqlConnection                             conn;
        // 정적 배정에서만 쓴다. 동적 배정은 공유 배정기를 쓴다
        core::SwapQueue<std::unique_ptr<Request>>   queue;
        std::atomic<std::size_t>                    depth{0};
        std::atomic<std::uint64_t>                  handled{0};
        std::atomic<bool>                           signaled{false};
        std::atomic<bool>                           connected{false};
        std::mutex                                  mutex;
        std::condition_variable                     cv;
        std::thread                                 thread;
    };

    void run(Worker& worker, int index);
    void process(Worker& worker, std::unique_ptr<Request> req);

    // 모아 둔 `Save` 를 한 트랜잭션으로 실행하고 응답을 낸다.
    // 처리한 건수를 돌려주고 모아 둔 목록을 비운다
    std::size_t flush_saves(Worker& worker, std::vector<std::unique_ptr<Request>>& saves);
    // 배치에 넣어도 되는 요청인가. 기한을 넘겼으면 넣지 않는다
    bool batchable(const Request& req) const;
    // 배정 방식에 따라 처리 완료를 알린다
    void release(Worker& worker, const std::string& session_key);
    void handle(Worker& worker, Request& req, Response& res);
    bool run_once(Worker& worker, Request& req, Response& res);
    void notify(Worker& worker);
    void wake_shared(bool all);
    void reject(std::unique_ptr<Request> req);

    std::vector<std::unique_ptr<Worker>> workers_;
    ConnectionConfig                     cfg_{};
    ResponseSink                         sink_;
    std::atomic<bool>                    running_{false};
    std::atomic<bool>                    draining_{false};

    // 동적 배정. 워커가 공유한다
    Dispatch                             dispatch_   = Dispatch::Static;
    std::size_t                          batch_size_ = 1;
    RequestDispatcher                    shared_;
    std::mutex                           shared_mutex_;
    std::condition_variable              shared_cv_;
};

} // namespace db
