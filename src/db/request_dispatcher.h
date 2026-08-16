#pragma once
//
//  요청 배정 - 처리 중인 키를 피해 다음 요청을 고른다.
//
//  키를 워커에 미리 묶지 않는다. 대신 "지금 처리 중인 키" 를 표시해 두고
//  그 키의 다음 요청은 앞선 처리가 끝날 때까지 내보내지 않는다.
//  같은 키가 두 워커에서 동시에 돌지 않으므로 순서는 그대로 보장되고,
//  묶어 두지 않으므로 한가한 워커가 다음 키를 집을 수 있다.
//
//  해시로 워커를 고정하는 방식과 비교하면 -
//    얻는 것: 부하가 키마다 다를 때 워커가 놀지 않는다
//    내는 것: 배정할 때마다 잠금을 잡고, 키별 상태를 유지해야 한다
//
//  키마다 줄을 따로 둔다. 목록 하나를 훑는 방식이면
//  처리 중인 키가 앞을 막을 때 그만큼 훑는 길이가 늘어난다.
//
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "db/db_types.h"

namespace db {

class RequestDispatcher {
public:
    RequestDispatcher() = default;

    RequestDispatcher(const RequestDispatcher&)            = delete;
    RequestDispatcher& operator=(const RequestDispatcher&) = delete;

    // 상한을 넘으면 받지 않는다. 거절 판단은 호출부가 한다
    bool push(std::unique_ptr<Request> req, std::size_t cap);

    // 지금 내보낼 수 있는 요청 하나. 없으면 nullptr.
    // 꺼낸 키는 finish 를 부를 때까지 다시 나오지 않는다
    std::unique_ptr<Request> take();

    // 처리가 끝났음을 알린다. 밀려 있던 같은 키가 있으면 다시 내보낼 수 있게 된다
    void finish(const std::string& key);

    // 받아서 아직 응답하지 않은 요청 수. 처리 중인 것도 포함한다
    std::size_t depth() const;

    // 지금 내보낼 수 있는 요청이 있는가.
    // 대기 여부 판단에 depth 를 쓰면, 다른 워커가 처리 중인 동안에도
    // 깨어나 빈손으로 돌아오는 일이 반복된다
    bool ready() const;

    // 종료 시 대기분을 모두 회수한다. 처리 중인 것은 그 워커가 끝낸다
    std::vector<std::unique_ptr<Request>> take_all();

private:
    struct KeyQueue {
        std::deque<std::unique_ptr<Request>> waiting;
        bool                                 working = false;
    };

    mutable std::mutex                          mutex_;
    std::unordered_map<std::string, KeyQueue>   queues_;
    // 처리 중이 아니면서 대기분이 있는 키만 들어온다
    std::deque<std::string>                     ready_;
    std::size_t                                 depth_ = 0;
};

} // namespace db
