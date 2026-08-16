#include "db/db_worker_pool.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "core/clock.h"
#include "core/log.h"

namespace db {
namespace {

// 재연결 시도 간격. 너무 짧으면 실패 로그만 쌓인다
constexpr int kReconnectIntervalMs = 1000;

// 유휴 커넥션 생존 확인 주기.
// 끊김은 질의를 보내야 드러난다. 확인하지 않으면 다음 요청 한 건이
// 그대로 사용자 오류가 된다
constexpr int kIdlePingIntervalMs = 3000;

// 재시도해도 안전한 요청인지 본다.
//   History  - 읽기라 몇 번을 해도 같다
//   Counter  - 멱등성 키가 있어 두 번 반영되지 않는다
//   Save     - 앞선 시도가 커밋됐는지 알 수 없다. 재시도하면 행이 늘 수 있다
bool is_retry_safe(RequestKind kind) {
    return kind == RequestKind::History || kind == RequestKind::Counter;
}

std::size_t shard_of(const std::string& key, std::size_t count) {
    return std::hash<std::string>{}(key) % count;
}

} // namespace

WorkerPool::~WorkerPool() {
    stop();
}

Dispatch WorkerPool::parse_dispatch(const std::string& name) {
    return name == "dynamic" ? Dispatch::Dynamic : Dispatch::Static;
}

const char* WorkerPool::dispatch_name(Dispatch d) {
    return d == Dispatch::Dynamic ? "dynamic" : "static";
}

bool WorkerPool::start(const ConnectionConfig& cfg, int worker_count, ResponseSink sink,
                       Dispatch dispatch, int batch_size) {
    if (running_.load(std::memory_order_acquire)) return true;
    if (!sink) return false;

    // 0 은 "DB 를 쓰지 않는다" 는 뜻이다. 하한을 1 로 올려 버리면
    // 워커는 도는데 헬스 검사는 DB 를 건너뛰어, 둘이 서로 다른 말을 하게 된다
    if (worker_count == 0) {
        LOG_INFO("db worker pool disabled - worker_count=0");
        return true;
    }

    cfg_        = cfg;
    sink_       = std::move(sink);
    dispatch_   = dispatch;
    batch_size_ = static_cast<std::size_t>(std::clamp(batch_size, 1, 256));

    const int count = std::clamp(worker_count, 1, 64);
    running_.store(true, std::memory_order_release);

    workers_.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        workers_.push_back(std::make_unique<Worker>());
    }
    for (int i = 0; i < count; ++i) {
        Worker& w = *workers_[static_cast<std::size_t>(i)];
        w.thread = std::thread([this, &w, i] { run(w, i); });
    }

    LOG_INFO("db worker pool started workers=%d dispatch=%s batch=%zu",
             count, dispatch_name(dispatch_), batch_size_);
    return true;
}

void WorkerPool::drain(int timeout_ms) {
    if (!running_.load(std::memory_order_acquire)) return;

    // 여기서부터 들어오는 요청은 거절한다. 받아 두면 끝이 없다
    draining_.store(true, std::memory_order_release);
    for (auto& w : workers_) notify(*w);
    wake_shared(true);

    const std::int64_t deadline = core::now_ms() + std::max(0, timeout_ms);
    while (queued() > 0 && core::now_ms() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const std::size_t left = queued();
    if (left > 0) {
        LOG_WARN("db drain timeout - %zu requests remain", left);
    } else {
        LOG_INFO("db drain done");
    }
}

void WorkerPool::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;

    for (auto& w : workers_) {
        notify(*w);
    }
    wake_shared(true);

    for (auto& w : workers_) {
        if (w->thread.joinable()) w->thread.join();
        w->conn.close();
    }

    // 공유 배정기에 남은 요청도 그냥 버리지 않는다
    if (dispatch_ == Dispatch::Dynamic) {
        for (auto& req : shared_.take_all()) reject(std::move(req));
    }

    // 배정이 얼마나 고르게 됐는지는 이 값의 편차로 드러난다
    for (std::size_t i = 0; i < workers_.size(); ++i) {
        LOG_INFO("db worker %zu handled=%llu", i,
                 static_cast<unsigned long long>(
                     workers_[i]->handled.load(std::memory_order_relaxed)));
    }

    workers_.clear();
    LOG_INFO("db worker pool stopped dispatch=%s", dispatch_name(dispatch_));
}

void WorkerPool::notify(Worker& worker) {
    if (worker.signaled.exchange(true, std::memory_order_release)) return;
    {
        std::lock_guard<std::mutex> guard(worker.mutex);
    }
    worker.cv.notify_one();
}

void WorkerPool::wake_shared(bool all) {
    if (dispatch_ != Dispatch::Dynamic) return;

    // 잠금을 한 번 잡았다 놓아야 대기에 막 들어가려던 워커가 신호를 놓치지 않는다
    {
        std::lock_guard<std::mutex> guard(shared_mutex_);
    }
    if (all) {
        shared_cv_.notify_all();
    } else {
        shared_cv_.notify_one();
    }
}

void WorkerPool::reject(std::unique_ptr<Request> req) {
    if (!req) return;

    auto res = std::make_unique<Response>();
    res->kind         = req->kind;
    res->conn_id      = req->conn_id;
    res->submitted_us = req->submitted_us;
    res->status       = Status::Unavailable;
    sink_(std::move(res));
}

bool WorkerPool::submit(std::unique_ptr<Request> req) {
    if (!running_.load(std::memory_order_acquire) || workers_.empty()) return false;
    if (draining_.load(std::memory_order_acquire)) return false;
    if (!req) return false;

    // 어느 방식이든 상한은 같다. 총량이 달라지면 두 경로를 나란히 잴 수 없다
    if (dispatch_ == Dispatch::Dynamic) {
        const std::size_t cap = kMaxQueuePerWorker * workers_.size();
        if (!shared_.push(std::move(req), cap)) return false;
        wake_shared(false);
        return true;
    }

    Worker& w = *workers_[shard_of(req->session_key, workers_.size())];

    // 무한히 쌓지 않는다. 넘치면 거절하고 호출부가 오류로 응답한다
    if (w.depth.load(std::memory_order_relaxed) >= kMaxQueuePerWorker) return false;

    w.depth.fetch_add(1, std::memory_order_relaxed);
    w.queue.push(std::move(req));
    notify(w);
    return true;
}

std::size_t WorkerPool::queued() const {
    if (dispatch_ == Dispatch::Dynamic) return shared_.depth();

    std::size_t total = 0;
    for (const auto& w : workers_) total += w->depth.load(std::memory_order_relaxed);
    return total;
}

int WorkerPool::connected_workers() const {
    int n = 0;
    for (const auto& w : workers_) {
        if (w->connected.load(std::memory_order_relaxed)) ++n;
    }
    return n;
}

void WorkerPool::run(Worker& worker, int index) {
    // 클라이언트 라이브러리를 쓰는 스레드마다 필요하다
    thread_init();

    std::int64_t next_retry_ms = 0;
    std::int64_t next_ping_ms  = core::now_ms() + kIdlePingIntervalMs;

    // 기동 직후 한 번 붙어 본다. 실패해도 계속 돈다
    worker.connected.store(worker.conn.open(cfg_), std::memory_order_relaxed);
    if (!worker.connected.load(std::memory_order_relaxed)) {
        LOG_WARN("db worker %d initial connect failed: %s", index, worker.conn.last_error());
        next_retry_ms = core::now_ms() + kReconnectIntervalMs;
    }

    while (running_.load(std::memory_order_acquire)) {
        worker.signaled.store(false, std::memory_order_release);

        // 끊겨 있으면 주기적으로 다시 붙는다.
        // 자동 재연결을 쓰지 않으므로 구문 준비까지 여기서 다시 한다
        if (!worker.conn.usable()) {
            const std::int64_t now = core::now_ms();
            if (now >= next_retry_ms) {
                if (worker.conn.ensure_usable()) {
                    worker.connected.store(true, std::memory_order_relaxed);
                    LOG_INFO("db worker %d reconnected", index);
                } else {
                    worker.connected.store(false, std::memory_order_relaxed);
                    next_retry_ms = now + kReconnectIntervalMs;
                }
            }
        }

        // 요청이 없는 동안 커넥션이 살아 있는지 확인한다
        if (worker.conn.usable() && core::now_ms() >= next_ping_ms) {
            next_ping_ms = core::now_ms() + kIdlePingIntervalMs;
            if (!worker.conn.ping()) {
                worker.connected.store(false, std::memory_order_relaxed);
                next_retry_ms = 0;   // 곧바로 다시 붙는다
            }
        }

        std::size_t done = 0;

        // 연달아 나오는 Save 를 모았다가 한 트랜잭션으로 커밋한다.
        // 기다렸다 모으지 않는다 - 지금 큐에 있는 것만 묶으므로
        // 한가할 때는 배치가 1 이 되고 밀릴 때만 커진다
        std::vector<std::unique_ptr<Request>> saves;
        if (batch_size_ > 1) saves.reserve(batch_size_);

        if (dispatch_ == Dispatch::Static) {
            auto& batch = worker.queue.swap();
            for (auto& req : batch) {
                if (!req) continue;

                if (batchable(*req)) {
                    saves.push_back(std::move(req));
                    if (saves.size() >= batch_size_) done += flush_saves(worker, saves);
                    continue;
                }

                // 순서를 지키려면 모아 둔 것을 먼저 내보낸다.
                // 같은 키의 Save 가 아직 커밋되지 않았는데 History 를 실행하면
                // 그 결과에 방금 저장한 것이 빠진다
                done += flush_saves(worker, saves);

                process(worker, std::move(req));
                worker.depth.fetch_sub(1, std::memory_order_relaxed);
                ++done;
            }
            done += flush_saves(worker, saves);
            batch.clear();
        } else {
            // 한 건씩 가져간다. 여러 건을 한꺼번에 쥐면 쥐고 있는 동안 다른 워커가 논다
            while (auto req = shared_.take()) {
                if (batchable(*req)) {
                    saves.push_back(std::move(req));
                    if (saves.size() >= batch_size_) done += flush_saves(worker, saves);
                    if (!running_.load(std::memory_order_acquire)) break;
                    continue;
                }

                done += flush_saves(worker, saves);

                const std::string key = req->session_key;
                process(worker, std::move(req));

                // 이 키를 기다리던 요청이 풀렸을 수 있다
                shared_.finish(key);
                wake_shared(false);
                ++done;

                if (!running_.load(std::memory_order_acquire)) break;
            }
            done += flush_saves(worker, saves);
        }
        if (done > 0) next_ping_ms = core::now_ms() + kIdlePingIntervalMs;

        if (worker.signaled.load(std::memory_order_acquire)) continue;
        if (!running_.load(std::memory_order_acquire)) break;

        // 끊긴 상태면 재시도 시각까지만 기다린다
        int wait_ms = 50;
        if (!worker.conn.usable()) {
            wait_ms = std::max(1, static_cast<int>(next_retry_ms - core::now_ms()));
            wait_ms = std::min(wait_ms, kReconnectIntervalMs);
        }

        if (dispatch_ == Dispatch::Static) {
            std::unique_lock<std::mutex> lock(worker.mutex);
            worker.cv.wait_for(lock, std::chrono::milliseconds(wait_ms), [this, &worker] {
                return worker.signaled.load(std::memory_order_acquire) ||
                       !running_.load(std::memory_order_acquire);
            });
        } else {
            std::unique_lock<std::mutex> lock(shared_mutex_);
            shared_cv_.wait_for(lock, std::chrono::milliseconds(wait_ms), [this] {
                return shared_.ready() || !running_.load(std::memory_order_acquire);
            });
        }
    }

    // 종료 직전 남은 요청에 응답한다. 기다리는 쪽을 그냥 버리지 않는다.
    // 동적 배정은 공유분이라 풀이 한 번에 회수한다
    if (dispatch_ == Dispatch::Static) {
        auto& tail = worker.queue.swap();
        for (auto& req : tail) {
            if (!req) continue;
            worker.depth.fetch_sub(1, std::memory_order_relaxed);
            reject(std::move(req));
        }
        tail.clear();
    }

    thread_shutdown();
}

bool WorkerPool::batchable(const Request& req) const {
    if (batch_size_ <= 1) return false;

    // Counter 는 자체 트랜잭션을 연다. 겹쳐 열 수 없다.
    // History 는 읽기라 커밋이 없어 묶어도 얻을 것이 없다
    if (req.kind != RequestKind::Save) return false;

    // 기한을 넘긴 요청은 실행하지 않는다. 묶으면 그 판정이 커밋 뒤로 밀린다
    if (req.deadline_us > 0 && core::now_us() > req.deadline_us) return false;
    return true;
}

void WorkerPool::release(Worker& worker, const std::string& session_key) {
    if (dispatch_ == Dispatch::Dynamic) {
        shared_.finish(session_key);
        wake_shared(false);
    } else {
        worker.depth.fetch_sub(1, std::memory_order_relaxed);
    }
}

std::size_t WorkerPool::flush_saves(Worker& worker,
                                    std::vector<std::unique_ptr<Request>>& saves) {
    if (saves.empty()) return 0;

    const std::size_t count = saves.size();

    // 한 건이면 묶을 것이 없다. 커밋 횟수가 같으므로 평소 경로로 보낸다
    if (count == 1) {
        auto req = std::move(saves.front());
        saves.clear();

        const std::string key = req->session_key;
        process(worker, std::move(req));
        release(worker, key);
        return 1;
    }

    const std::int64_t begin_us = core::now_us();

    std::vector<std::uint64_t> log_ids(count, 0);
    std::size_t done = 0;
    bool ok = worker.conn.begin_batch();

    if (ok) {
        for (; done < count; ++done) {
            if (!worker.conn.save(saves[done]->session_key, saves[done]->payload, log_ids[done])) {
                ok = false;
                break;
            }
        }
    }
    if (ok) ok = worker.conn.commit_batch();

    if (!ok) {
        // 하나가 실패하면 전체가 되돌아간다. 묶는다는 것이 그런 뜻이다
        worker.conn.rollback_batch();
        LOG_WARN("db batch failed at %zu/%zu err=%u %s",
                 done, count, worker.conn.last_errno(), worker.conn.last_error());
    }

    // 커밋 하나를 나눠 가졌으므로 요청당 시간도 나눠 적는다
    const std::int64_t per_us =
        (core::now_us() - begin_us) / static_cast<std::int64_t>(count);
    const Status status = ok ? Status::Ok
                             : (worker.conn.usable() ? Status::Failed : Status::Unavailable);

    for (std::size_t i = 0; i < count; ++i) {
        auto res = std::make_unique<Response>();
        res->kind         = saves[i]->kind;
        res->conn_id      = saves[i]->conn_id;
        res->submitted_us = saves[i]->submitted_us;
        res->status       = status;
        res->log_id       = ok ? log_ids[i] : 0;
        res->db_us        = per_us;

        worker.handled.fetch_add(1, std::memory_order_relaxed);
        sink_(std::move(res));
    }

    for (auto& req : saves) release(worker, req->session_key);
    saves.clear();
    return count;
}

void WorkerPool::process(Worker& worker, std::unique_ptr<Request> req) {
    auto res = std::make_unique<Response>();
    res->kind         = req->kind;
    res->conn_id      = req->conn_id;
    res->submitted_us = req->submitted_us;

    const std::int64_t begin_us = core::now_us();
    handle(worker, *req, *res);
    res->db_us = core::now_us() - begin_us;

    worker.handled.fetch_add(1, std::memory_order_relaxed);
    sink_(std::move(res));
}

bool WorkerPool::run_once(Worker& worker, Request& req, Response& res) {
    switch (req.kind) {
        case RequestKind::Save:
            return worker.conn.save(req.session_key, req.payload, res.log_id);
        case RequestKind::History:
            return worker.conn.history(req.session_key, req.limit, res.rows);
        case RequestKind::Counter:
            return worker.conn.counter(req.session_key, req.request_key,
                                       res.hit_count, res.already_applied);
    }
    return false;
}

void WorkerPool::handle(Worker& worker, Request& req, Response& res) {
    // 기한을 넘긴 요청은 질의를 시작하지 않는다.
    // 밀린 구간에서 이 판정이 없으면 앞선 요청이 늦은 만큼
    // 뒤에 선 요청도 그대로 늦는다 - 줄 전체가 함께 무너진다
    if (req.deadline_us > 0 && core::now_us() > req.deadline_us) {
        res.status = Status::Expired;
        return;
    }

    if (!worker.conn.usable()) {
        res.status = Status::Unavailable;
        return;
    }

    if (run_once(worker, req, res)) {
        res.status = Status::Ok;
        return;
    }

    // 끊김으로 실패했고 재시도해도 안전한 요청이면 한 번만 다시 붙어 시도한다.
    // 커넥션이 죽은 사실을 이 요청이 처음 발견한 경우가 여기다
    const bool lost = !worker.conn.usable();
    if (lost && is_retry_safe(req.kind)) {
        if (worker.conn.ensure_usable()) {
            worker.connected.store(true, std::memory_order_relaxed);
            if (run_once(worker, req, res)) {
                res.status = Status::Ok;
                LOG_INFO("db retried after reconnect kind=%d", static_cast<int>(req.kind));
                return;
            }
        }
    }

    res.status = worker.conn.usable() ? Status::Failed : Status::Unavailable;
    LOG_WARN("db query failed kind=%d err=%u %s",
             static_cast<int>(req.kind), worker.conn.last_errno(), worker.conn.last_error());
}

} // namespace db
