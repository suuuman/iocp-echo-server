#include "db/mysql_connection.h"

#include <errmsg.h>
#include <mysqld_error.h>

#include <cstring>

#include "core/log.h"

namespace db {
namespace {

constexpr const char* kSqlSave =
    "INSERT INTO echo_log(session_key, payload) VALUES(?, ?)";

constexpr const char* kSqlHistory =
    "SELECT log_id, payload, CAST(UNIX_TIMESTAMP(created_at) * 1000 AS UNSIGNED) "
    "FROM echo_log WHERE session_key = ? ORDER BY log_id DESC LIMIT ?";

// 중복이면 여기서 실패한다. 그 실패가 곧 "이미 반영됨" 판정이다
constexpr const char* kSqlCounterClaim =
    "INSERT INTO counter_request(request_key, session_key) VALUES(?, ?)";

constexpr const char* kSqlCounterBump =
    "INSERT INTO echo_counter(session_key, hit_count, version) VALUES(?, 1, 1) "
    "ON DUPLICATE KEY UPDATE hit_count = hit_count + 1, version = version + 1";

constexpr const char* kSqlCounterRead =
    "SELECT hit_count FROM echo_counter WHERE session_key = ?";

// 저장 프로시저 경로. 한 번의 호출로 끝난다
constexpr const char* kCallSave =
    "CALL sp_save_log(?, ?, ?)";
constexpr const char* kCallHistory =
    "CALL sp_get_history(?, ?)";
constexpr const char* kCallCounter =
    "CALL sp_apply_counter(?, ?, ?, ?)";

constexpr unsigned long kMaxPayloadLen = 512;

// 연결이 끊긴 종류인지 본다. 이 경우에만 재연결로 넘어간다
bool is_connection_lost(unsigned err) {
    return err == CR_SERVER_GONE_ERROR || err == CR_SERVER_LOST ||
           err == CR_CONN_HOST_ERROR   || err == CR_SERVER_LOST_EXTENDED;
}

void bind_string(MYSQL_BIND& b, const char* buf, unsigned long& len) {
    std::memset(&b, 0, sizeof(b));
    b.buffer_type   = MYSQL_TYPE_STRING;
    b.buffer        = const_cast<char*>(buf);
    b.buffer_length = len;
    b.length        = &len;
}

void bind_uint64(MYSQL_BIND& b, unsigned long long& value) {
    std::memset(&b, 0, sizeof(b));
    b.buffer_type = MYSQL_TYPE_LONGLONG;
    b.buffer      = &value;
    b.is_unsigned = 1;
}

} // namespace

bool library_init() {
    return ::mysql_library_init(0, nullptr, nullptr) == 0;
}

void library_shutdown() {
    ::mysql_library_end();
}

void thread_init() {
    ::mysql_thread_init();
}

void thread_shutdown() {
    ::mysql_thread_end();
}

MysqlConnection::~MysqlConnection() {
    close();
}

bool MysqlConnection::open(const ConnectionConfig& cfg) {
    cfg_ = cfg;
    close();

    mysql_ = ::mysql_init(nullptr);
    if (mysql_ == nullptr) {
        last_error_ = "mysql_init failed";
        return false;
    }

    const unsigned timeout_sec = cfg_.timeout_ms > 0 ? (cfg_.timeout_ms + 999) / 1000 : 5;
    ::mysql_options(mysql_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_sec);
    ::mysql_options(mysql_, MYSQL_OPT_READ_TIMEOUT, &timeout_sec);
    ::mysql_options(mysql_, MYSQL_OPT_WRITE_TIMEOUT, &timeout_sec);
    ::mysql_options(mysql_, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    // 자동 재연결은 켜지 않는다. 준비된 구문이 조용히 무효화된다.
    // CLIENT_MULTI_RESULTS - CALL 은 결과셋을 여러 개 돌려준다

    if (::mysql_real_connect(mysql_, cfg_.host.c_str(), cfg_.user.c_str(),
                             cfg_.password.c_str(), cfg_.schema.c_str(),
                             cfg_.port, nullptr, CLIENT_MULTI_RESULTS) == nullptr) {
        last_errno_ = ::mysql_errno(mysql_);
        last_error_ = ::mysql_error(mysql_);
        ::mysql_close(mysql_);
        mysql_ = nullptr;
        return false;
    }

    if (!prepare_all()) {
        close();
        return false;
    }

    broken_ = false;
    LOG_INFO("db connected %s:%u/%s", cfg_.host.c_str(), cfg_.port, cfg_.schema.c_str());
    return true;
}

void MysqlConnection::close() {
    close_statements();
    if (mysql_ != nullptr) {
        ::mysql_close(mysql_);
        mysql_ = nullptr;
    }
    broken_ = true;
}

bool MysqlConnection::ping() {
    if (mysql_ == nullptr || broken_) return false;

    if (::mysql_ping(mysql_) != 0) {
        last_errno_ = ::mysql_errno(mysql_);
        last_error_ = ::mysql_error(mysql_);
        LOG_WARN("db ping failed (%u: %s)", last_errno_, last_error_.c_str());
        broken_ = true;
        return false;
    }
    return true;
}

bool MysqlConnection::ensure_usable() {
    if (usable()) return true;
    return open(cfg_);
}

void MysqlConnection::close_statements() {
    MYSQL_STMT** all[] = {&st_save_, &st_history_, &st_counter_claim_,
                          &st_counter_bump_, &st_counter_read_,
                          &st_proc_save_, &st_proc_history_, &st_proc_counter_};
    for (auto* slot : all) {
        if (*slot != nullptr) {
            ::mysql_stmt_close(*slot);
            *slot = nullptr;
        }
    }
}

bool MysqlConnection::prepare_all() {
    struct Item { MYSQL_STMT** slot; const char* sql; };

    // 쓰지 않는 경로의 구문은 준비하지 않는다.
    // 재연결마다 다섯 개를 헛되이 준비할 이유가 없다
    const Item direct_items[] = {
        {&st_save_,          kSqlSave},
        {&st_history_,       kSqlHistory},
        {&st_counter_claim_, kSqlCounterClaim},
        {&st_counter_bump_,  kSqlCounterBump},
        {&st_counter_read_,  kSqlCounterRead},
    };
    const Item proc_items[] = {
        {&st_proc_save_,    kCallSave},
        {&st_proc_history_, kCallHistory},
        {&st_proc_counter_, kCallCounter},
    };

    const Item* items = cfg_.use_procedures ? proc_items : direct_items;
    const std::size_t count =
        cfg_.use_procedures ? std::size(proc_items) : std::size(direct_items);

    for (std::size_t i = 0; i < count; ++i) {
        const auto& item = items[i];
        MYSQL_STMT* stmt = ::mysql_stmt_init(mysql_);
        if (stmt == nullptr) {
            last_error_ = "mysql_stmt_init failed";
            return false;
        }
        if (::mysql_stmt_prepare(stmt, item.sql, static_cast<unsigned long>(std::strlen(item.sql))) != 0) {
            last_errno_ = ::mysql_stmt_errno(stmt);
            last_error_ = ::mysql_stmt_error(stmt);
            LOG_ERROR("prepare failed: %s (%s)", last_error_.c_str(), item.sql);
            ::mysql_stmt_close(stmt);
            return false;
        }
        *item.slot = stmt;
    }
    return true;
}

void MysqlConnection::note_failure(MYSQL_STMT* stmt) {
    last_errno_ = ::mysql_stmt_errno(stmt);
    last_error_ = ::mysql_stmt_error(stmt);

    if (is_connection_lost(last_errno_)) {
        // 준비된 구문이 모두 무효가 됐다. 다음 요청에서 새로 연결하고 다시 준비한다
        LOG_WARN("db connection lost (%u: %s)", last_errno_, last_error_.c_str());
        broken_ = true;
    }
}

bool MysqlConnection::exec(MYSQL_STMT* stmt, MYSQL_BIND* params) {
    if (params != nullptr && ::mysql_stmt_bind_param(stmt, params) != 0) {
        note_failure(stmt);
        return false;
    }
    if (::mysql_stmt_execute(stmt) != 0) {
        note_failure(stmt);
        return false;
    }
    return true;
}

void MysqlConnection::note_failure() {
    if (mysql_ == nullptr) return;
    last_errno_ = ::mysql_errno(mysql_);
    last_error_ = ::mysql_error(mysql_);
    if (is_connection_lost(last_errno_)) broken_ = true;
}

bool MysqlConnection::run_plain(const char* sql) {
    if (::mysql_real_query(mysql_, sql, static_cast<unsigned long>(std::strlen(sql))) != 0) {
        note_failure();
        return false;
    }
    return true;
}

// -------------------------------------------------------------------
//  묶어 커밋하기
// -------------------------------------------------------------------
bool MysqlConnection::begin_batch() {
    if (!usable()) return false;
    return run_plain("START TRANSACTION");
}

bool MysqlConnection::commit_batch() {
    if (!usable()) return false;
    if (::mysql_commit(mysql_) != 0) {
        note_failure();
        return false;
    }
    return true;
}

void MysqlConnection::rollback_batch() noexcept {
    // 끊긴 커넥션에도 불러 둔다. 되살아난 뒤 열린 트랜잭션이 남아 있으면
    // 다음 요청이 그 안에서 실행된다
    if (mysql_ != nullptr) ::mysql_rollback(mysql_);
}

// -------------------------------------------------------------------
//  Save
// -------------------------------------------------------------------
bool MysqlConnection::save_direct(const std::string& session_key, const std::string& payload,
                                  std::uint64_t& out_log_id) {
    out_log_id = 0;
    if (!usable()) return false;

    unsigned long key_len     = static_cast<unsigned long>(session_key.size());
    unsigned long payload_len = static_cast<unsigned long>(payload.size());

    MYSQL_BIND params[2]{};
    bind_string(params[0], session_key.c_str(), key_len);
    bind_string(params[1], payload.c_str(), payload_len);

    if (!exec(st_save_, params)) return false;

    out_log_id = ::mysql_stmt_insert_id(st_save_);
    return true;
}

// -------------------------------------------------------------------
//  History
// -------------------------------------------------------------------
bool MysqlConnection::history_direct(const std::string& session_key, unsigned limit,
                                     std::vector<LogRow>& out_rows) {
    out_rows.clear();
    if (!usable()) return false;

    unsigned long      key_len   = static_cast<unsigned long>(session_key.size());
    unsigned long long limit_val = limit;

    MYSQL_BIND params[2]{};
    bind_string(params[0], session_key.c_str(), key_len);
    bind_uint64(params[1], limit_val);

    if (!exec(st_history_, params)) return false;
    return fetch_history_rows(st_history_, out_rows);
}

// 결과셋을 행으로 옮긴다. 직접 SQL 경로와 프로시저 경로가 같은 형태를 돌려주므로
// 매핑을 한 곳에 둔다
bool MysqlConnection::fetch_history_rows(MYSQL_STMT* stmt, std::vector<LogRow>& out_rows) {
    unsigned long long log_id = 0;
    unsigned long long ms     = 0;
    char               payload[kMaxPayloadLen + 1]{};
    unsigned long      payload_len  = 0;
    // MySQL 8.0 에서 my_bool 이 없어졌다. is_null 은 bool* 를 받는다
    bool               payload_null = false;

    MYSQL_BIND cols[3]{};
    bind_uint64(cols[0], log_id);
    std::memset(&cols[1], 0, sizeof(cols[1]));
    cols[1].buffer_type   = MYSQL_TYPE_STRING;
    cols[1].buffer        = payload;
    cols[1].buffer_length = kMaxPayloadLen;
    cols[1].length        = &payload_len;
    cols[1].is_null       = &payload_null;
    bind_uint64(cols[2], ms);

    if (::mysql_stmt_bind_result(stmt, cols) != 0) {
        note_failure(stmt);
        return false;
    }
    // 결과를 먼저 받아 둔다. 커넥션을 붙잡은 채로 행을 하나씩 끌어오지 않는다
    if (::mysql_stmt_store_result(stmt) != 0) {
        note_failure(stmt);
        return false;
    }

    for (;;) {
        const int rc = ::mysql_stmt_fetch(stmt);
        if (rc == MYSQL_NO_DATA) break;
        if (rc != 0 && rc != MYSQL_DATA_TRUNCATED) {
            note_failure(stmt);
            ::mysql_stmt_free_result(stmt);
            return false;
        }

        LogRow row;
        row.log_id        = log_id;
        row.created_at_ms = ms;
        if (!payload_null) {
            row.payload.assign(payload, payload_len > kMaxPayloadLen ? kMaxPayloadLen : payload_len);
        }
        out_rows.push_back(std::move(row));
    }

    ::mysql_stmt_free_result(stmt);
    return true;
}

// CALL 은 프로시저가 만든 결과셋 뒤에 종료 상태를 하나 더 보낸다.
// 비워 두지 않으면 다음 질의가 "명령 순서 오류" 로 실패한다
void MysqlConnection::drain_extra_results(MYSQL_STMT* stmt) {
    for (;;) {
        const int rc = ::mysql_stmt_next_result(stmt);
        if (rc != 0) break;              // -1 = 더 없음, >0 = 오류
        ::mysql_stmt_free_result(stmt);
    }
}

// -------------------------------------------------------------------
//  Counter
//
//  같은 request_key 가 다시 오면 오류가 아니라 "이미 반영됨" 으로 응답한다.
//  응답을 받지 못한 클라이언트가 재시도하는 것은 정상이고,
//  서버는 그 재시도에 같은 결과를 돌려주어야 한다.
// -------------------------------------------------------------------
bool MysqlConnection::counter_direct(const std::string& session_key, const std::string& request_key,
                                     std::uint64_t& out_hit_count, bool& out_already_applied) {
    out_hit_count       = 0;
    out_already_applied = false;
    if (!usable()) return false;

    unsigned long key_len = static_cast<unsigned long>(session_key.size());
    unsigned long req_len = static_cast<unsigned long>(request_key.size());

    if (!run_plain("START TRANSACTION")) return false;

    // 1) 멱등성 키 선점
    MYSQL_BIND claim[2]{};
    bind_string(claim[0], request_key.c_str(), req_len);
    bind_string(claim[1], session_key.c_str(), key_len);

    if (!exec(st_counter_claim_, claim)) {
        const bool duplicate = (last_errno_ == ER_DUP_ENTRY);
        ::mysql_rollback(mysql_);

        if (!duplicate) return false;

        // 이미 반영된 요청이다. 현재 값을 읽어 그대로 돌려준다
        out_already_applied = true;
        MYSQL_BIND read_param[1]{};
        unsigned long k = key_len;
        bind_string(read_param[0], session_key.c_str(), k);

        if (!exec(st_counter_read_, read_param)) return false;

        unsigned long long value = 0;
        MYSQL_BIND col[1]{};
        bind_uint64(col[0], value);

        if (::mysql_stmt_bind_result(st_counter_read_, col) != 0 ||
            ::mysql_stmt_store_result(st_counter_read_) != 0) {
            note_failure(st_counter_read_);
            return false;
        }
        if (::mysql_stmt_fetch(st_counter_read_) == 0) out_hit_count = value;
        ::mysql_stmt_free_result(st_counter_read_);
        return true;
    }

    // 2) 증가
    MYSQL_BIND bump[1]{};
    unsigned long bump_len = key_len;
    bind_string(bump[0], session_key.c_str(), bump_len);

    if (!exec(st_counter_bump_, bump)) {
        ::mysql_rollback(mysql_);
        return false;
    }

    // 3) 반영된 값 확인
    MYSQL_BIND read_param[1]{};
    unsigned long read_len = key_len;
    bind_string(read_param[0], session_key.c_str(), read_len);

    if (!exec(st_counter_read_, read_param)) {
        ::mysql_rollback(mysql_);
        return false;
    }

    unsigned long long value = 0;
    MYSQL_BIND col[1]{};
    bind_uint64(col[0], value);

    if (::mysql_stmt_bind_result(st_counter_read_, col) != 0 ||
        ::mysql_stmt_store_result(st_counter_read_) != 0) {
        note_failure(st_counter_read_);
        ::mysql_rollback(mysql_);
        return false;
    }
    const bool fetched = (::mysql_stmt_fetch(st_counter_read_) == 0);
    ::mysql_stmt_free_result(st_counter_read_);

    if (!fetched) {
        ::mysql_rollback(mysql_);
        return false;
    }

    if (::mysql_commit(mysql_) != 0) {
        last_errno_ = ::mysql_errno(mysql_);
        last_error_ = ::mysql_error(mysql_);
        if (is_connection_lost(last_errno_)) broken_ = true;
        return false;
    }

    out_hit_count = value;
    return true;
}

// -------------------------------------------------------------------
//  경로 분기
//
//  두 경로가 같은 결과를 내야 한다. 관문 검증은 양쪽 모두에 대해 돌린다
// -------------------------------------------------------------------
bool MysqlConnection::save(const std::string& session_key, const std::string& payload,
                           std::uint64_t& out_log_id) {
    out_log_id = 0;
    if (!usable()) return false;
    return cfg_.use_procedures ? save_proc(session_key, payload, out_log_id)
                               : save_direct(session_key, payload, out_log_id);
}

bool MysqlConnection::history(const std::string& session_key, unsigned limit,
                              std::vector<LogRow>& out_rows) {
    out_rows.clear();
    if (!usable()) return false;
    return cfg_.use_procedures ? history_proc(session_key, limit, out_rows)
                               : history_direct(session_key, limit, out_rows);
}

bool MysqlConnection::counter(const std::string& session_key, const std::string& request_key,
                              std::uint64_t& out_hit_count, bool& out_already_applied) {
    out_hit_count       = 0;
    out_already_applied = false;
    if (!usable()) return false;
    return cfg_.use_procedures
               ? counter_proc(session_key, request_key, out_hit_count, out_already_applied)
               : counter_direct(session_key, request_key, out_hit_count, out_already_applied);
}

// -------------------------------------------------------------------
//  저장 프로시저 경로
//
//  OUT 파라미터는 결과셋으로 돌아온다.
//  준비된 구문에서 CALL 을 쓰면 서버가 OUT 값을 한 행으로 실어 보내므로
//  bind_result 로 받는다. 별도 SELECT @변수 왕복이 필요 없다
// -------------------------------------------------------------------
bool MysqlConnection::save_proc(const std::string& session_key, const std::string& payload,
                                std::uint64_t& out_log_id) {
    unsigned long key_len     = static_cast<unsigned long>(session_key.size());
    unsigned long payload_len = static_cast<unsigned long>(payload.size());
    unsigned long long log_id = 0;

    MYSQL_BIND params[3]{};
    bind_string(params[0], session_key.c_str(), key_len);
    bind_string(params[1], payload.c_str(), payload_len);
    bind_uint64(params[2], log_id);
    params[2].buffer_type = MYSQL_TYPE_LONGLONG;   // OUT

    if (!exec(st_proc_save_, params)) return false;

    MYSQL_BIND out[1]{};
    bind_uint64(out[0], log_id);

    if (::mysql_stmt_bind_result(st_proc_save_, out) != 0 ||
        ::mysql_stmt_store_result(st_proc_save_) != 0) {
        note_failure(st_proc_save_);
        return false;
    }
    const bool fetched = (::mysql_stmt_fetch(st_proc_save_) == 0);
    ::mysql_stmt_free_result(st_proc_save_);

    drain_extra_results(st_proc_save_);

    if (!fetched) return false;
    out_log_id = log_id;
    return true;
}

bool MysqlConnection::history_proc(const std::string& session_key, unsigned limit,
                                   std::vector<LogRow>& out_rows) {
    unsigned long      key_len   = static_cast<unsigned long>(session_key.size());
    unsigned long long limit_val = limit;

    MYSQL_BIND params[2]{};
    bind_string(params[0], session_key.c_str(), key_len);
    bind_uint64(params[1], limit_val);

    if (!exec(st_proc_history_, params)) return false;
    if (!fetch_history_rows(st_proc_history_, out_rows)) return false;

    drain_extra_results(st_proc_history_);
    return true;
}

bool MysqlConnection::counter_proc(const std::string& session_key, const std::string& request_key,
                                   std::uint64_t& out_hit_count, bool& out_already_applied) {
    unsigned long key_len = static_cast<unsigned long>(session_key.size());
    unsigned long req_len = static_cast<unsigned long>(request_key.size());

    unsigned long long hit_count = 0;
    unsigned long long applied   = 0;

    MYSQL_BIND params[4]{};
    bind_string(params[0], request_key.c_str(), req_len);
    bind_string(params[1], session_key.c_str(), key_len);
    bind_uint64(params[2], hit_count);   // OUT
    bind_uint64(params[3], applied);     // OUT

    // 트랜잭션 · 멱등성 판정 · 값 조회가 이 한 번에 모두 들어 있다
    if (!exec(st_proc_counter_, params)) return false;

    MYSQL_BIND out[2]{};
    bind_uint64(out[0], hit_count);
    bind_uint64(out[1], applied);

    if (::mysql_stmt_bind_result(st_proc_counter_, out) != 0 ||
        ::mysql_stmt_store_result(st_proc_counter_) != 0) {
        note_failure(st_proc_counter_);
        return false;
    }
    const bool fetched = (::mysql_stmt_fetch(st_proc_counter_) == 0);
    ::mysql_stmt_free_result(st_proc_counter_);

    drain_extra_results(st_proc_counter_);

    if (!fetched) return false;
    out_hit_count       = hit_count;
    out_already_applied = (applied != 0);
    return true;
}

} // namespace db
