#pragma once
//
//  DB 요청과 응답.
//
//  요청은 로직 스레드가 만들고 워커가 소비한다.
//  응답은 워커가 만들고 로직 스레드가 소비한다.
//  두 방향 모두 소유권을 넘기므로 unique_ptr 로 다룬다.
//
#include <cstdint>
#include <string>
#include <vector>

#include "net/connection.h"

namespace db {

enum class RequestKind : std::uint8_t {
    Save,
    History,
    Counter,
};

enum class Status : std::uint8_t {
    Ok,
    Unavailable,   // 커넥션이 끊겨 있다. 재연결 진행 중
    Failed,        // 질의 자체가 실패했다
    Expired,       // 기한을 넘겼다. 질의를 시작하지 않았다
};

struct Request {
    RequestKind       kind    = RequestKind::Save;
    net::ConnectionId conn_id = 0;

    // 워커 배정 키이자 조회 키다.
    // 같은 값은 항상 같은 워커로 가므로 순서가 보장된다
    std::string session_key;

    std::string   payload;      // Save
    std::string   request_key;  // Counter
    std::uint16_t limit = 0;    // History

    std::int64_t submitted_us = 0;

    // 이 시각을 넘기면 실행하지 않는다. 0 이면 기한이 없다.
    //
    // 이미 늦은 질의를 실행해도 기다리던 쪽은 대개 받지 않는다.
    // 그 시간에 워커를 쓰면 뒤에 밀린 요청까지 함께 늦는다
    std::int64_t deadline_us = 0;
};

struct LogRow {
    std::uint64_t log_id        = 0;
    std::string   payload;
    std::uint64_t created_at_ms = 0;
};

struct Response {
    RequestKind       kind    = RequestKind::Save;
    net::ConnectionId conn_id = 0;
    Status            status  = Status::Ok;

    std::uint64_t       log_id = 0;   // Save
    std::vector<LogRow> rows;         // History

    std::uint64_t hit_count       = 0;      // Counter
    bool          already_applied = false;  // Counter - 중복 요청이었다

    std::int64_t db_us = 0;   // 질의에 쓴 시간
    // 요청이 큐에 들어간 시각. 질의 시간만으로는 대기 구간이 보이지 않는다
    std::int64_t submitted_us = 0;
};

} // namespace db
