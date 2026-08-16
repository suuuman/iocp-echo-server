#include "db/request_dispatcher.h"

#include <utility>

namespace db {

bool RequestDispatcher::push(std::unique_ptr<Request> req, std::size_t cap) {
    if (!req) return false;

    // 이동하고 나면 읽을 수 없다. 키를 먼저 확보한다
    const std::string key = req->session_key;

    std::lock_guard<std::mutex> guard(mutex_);
    if (depth_ >= cap) return false;

    KeyQueue& kq = queues_[key];
    kq.waiting.push_back(std::move(req));
    ++depth_;

    // 처리 중이 아니고 이번에 처음 쌓인 키만 내보낼 목록에 넣는다.
    // 이미 목록에 있는 키를 다시 넣으면 같은 키가 두 번 나간다
    if (!kq.working && kq.waiting.size() == 1) ready_.push_back(key);
    return true;
}

std::unique_ptr<Request> RequestDispatcher::take() {
    std::lock_guard<std::mutex> guard(mutex_);

    while (!ready_.empty()) {
        const std::string key = ready_.front();
        ready_.pop_front();

        const auto it = queues_.find(key);
        if (it == queues_.end()) continue;

        KeyQueue& kq = it->second;
        if (kq.working || kq.waiting.empty()) continue;

        kq.working = true;
        auto req = std::move(kq.waiting.front());
        kq.waiting.pop_front();
        return req;
    }
    return nullptr;
}

void RequestDispatcher::finish(const std::string& key) {
    std::lock_guard<std::mutex> guard(mutex_);

    const auto it = queues_.find(key);
    if (it == queues_.end()) return;

    KeyQueue& kq = it->second;
    kq.working = false;
    if (depth_ > 0) --depth_;

    // 대기분이 없으면 항목을 지운다. 두면 지나간 키가 표에 계속 남는다
    if (kq.waiting.empty()) {
        queues_.erase(it);
        return;
    }
    ready_.push_back(key);
}

std::size_t RequestDispatcher::depth() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return depth_;
}

bool RequestDispatcher::ready() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return !ready_.empty();
}

std::vector<std::unique_ptr<Request>> RequestDispatcher::take_all() {
    std::lock_guard<std::mutex> guard(mutex_);

    std::vector<std::unique_ptr<Request>> all;
    all.reserve(depth_);

    for (auto& entry : queues_) {
        KeyQueue& kq = entry.second;
        for (auto& req : kq.waiting) {
            all.push_back(std::move(req));
            if (depth_ > 0) --depth_;
        }
        kq.waiting.clear();
    }

    // 처리 중인 키는 그 워커가 끝내고 finish 를 부른다.
    // 여기서 표를 비우면 그 호출이 갈 곳을 잃으므로 표는 남긴다
    ready_.clear();
    return all;
}

} // namespace db
