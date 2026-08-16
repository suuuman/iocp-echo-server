#pragma once
//
//  MySQL 커넥션 하나를 감싼다. 워커 스레드 하나가 독점한다.
//
//  자동 재연결(MYSQL_OPT_RECONNECT)을 쓰지 않는다.
//  자동 재연결은 세션 상태를 조용히 버리고 준비된 구문을 전부 무효화한다.
//  끊김을 직접 감지해 다시 연결하고 구문을 다시 준비한다.
//
#include <mysql.h>

#include <cstdint>
#include <string>
#include <vector>

#include "db/db_types.h"

namespace db {

// 클라이언트 라이브러리 초기화. 스레드를 만들기 전에 메인에서 한 번 부른다.
// 여러 스레드가 mysql_init() 을 동시에 처음 호출하면 내부 초기화가 경합해
// Winsock 이 준비되기 전에 접속을 시도하는 일이 생긴다
bool library_init();
void library_shutdown();

// 라이브러리를 쓰는 스레드마다 진입 · 이탈 시 부른다
void thread_init();
void thread_shutdown();

struct ConnectionConfig {
    std::string  host       = "127.0.0.1";
    unsigned     port       = 3306;
    std::string  user;
    std::string  password;
    std::string  schema;
    unsigned     timeout_ms = 5000;

    // 같은 일을 하는 경로가 둘 있다.
    //   false - 애플리케이션이 구문을 나눠 보내고 트랜잭션도 직접 연다
    //   true  - 저장 프로시저 한 번으로 끝낸다
    // 왕복 횟수를 비교 측정하려고 둘 다 남겨 두었다
    bool use_procedures = false;
};

class MysqlConnection {
public:
    MysqlConnection() = default;
    ~MysqlConnection();

    MysqlConnection(const MysqlConnection&)            = delete;
    MysqlConnection& operator=(const MysqlConnection&) = delete;

    // 연결 후 구문을 준비한다. 실패해도 워커는 계속 돌며 재시도한다
    bool open(const ConnectionConfig& cfg);
    void close();

    bool usable() const noexcept { return mysql_ != nullptr && !broken_; }

    // 유휴 커넥션이 끊겼는지 확인한다.
    // 끊김은 질의를 보내야 드러나므로, 쓰지 않는 동안에는 이걸로 본다
    bool ping();
    // 끊겨 있으면 다시 연결하고 구문을 다시 준비한다
    bool ensure_usable();

    bool save(const std::string& session_key, const std::string& payload,
              std::uint64_t& out_log_id);

    // 여러 요청을 한 트랜잭션으로 묶는다.
    //
    // `Save` 하나에 드는 시간의 대부분은 질의가 아니라 커밋이다.
    // 가장 엄격한 지속성 설정에서는 커밋마다 디스크 동기화가 두 번 일어난다.
    // 묶으면 그 비용을 안에 든 요청들이 나눠 갖는다.
    //
    // 비용 - 하나가 실패하면 그 트랜잭션 전체가 되돌아간다.
    //        같은 배치에 든 다른 요청도 함께 실패한다
    bool begin_batch();
    bool commit_batch();
    void rollback_batch() noexcept;

    bool history(const std::string& session_key, unsigned limit,
                 std::vector<LogRow>& out_rows);

    // already_applied 는 같은 request_key 가 이미 반영된 경우다.
    // 이것은 오류가 아니라 판정 결과다
    bool counter(const std::string& session_key, const std::string& request_key,
                 std::uint64_t& out_hit_count, bool& out_already_applied);

    unsigned    last_errno() const noexcept { return last_errno_; }
    const char* last_error() const noexcept { return last_error_.c_str(); }

    // 이 커넥션이 어느 경로를 쓰고 있는지
    bool uses_procedures() const noexcept { return cfg_.use_procedures; }

private:
    bool prepare_all();
    void close_statements();

    // 직접 SQL 경로
    bool save_direct(const std::string& session_key, const std::string& payload,
                     std::uint64_t& out_log_id);
    bool history_direct(const std::string& session_key, unsigned limit,
                        std::vector<LogRow>& out_rows);
    bool counter_direct(const std::string& session_key, const std::string& request_key,
                        std::uint64_t& out_hit_count, bool& out_already_applied);

    // 저장 프로시저 경로
    bool save_proc(const std::string& session_key, const std::string& payload,
                   std::uint64_t& out_log_id);
    bool history_proc(const std::string& session_key, unsigned limit,
                      std::vector<LogRow>& out_rows);
    bool counter_proc(const std::string& session_key, const std::string& request_key,
                      std::uint64_t& out_hit_count, bool& out_already_applied);

    // 결과셋을 행으로 옮긴다. 두 경로가 공유한다
    bool fetch_history_rows(MYSQL_STMT* stmt, std::vector<LogRow>& out_rows);
    // CALL 이 남기는 종료 상태 결과셋을 비운다
    void drain_extra_results(MYSQL_STMT* stmt);

    // 실패를 기록하고, 커넥션이 끊긴 종류면 broken 으로 표시한다
    void note_failure(MYSQL_STMT* stmt);
    // 구문이 아니라 커넥션 수준에서 난 실패
    void note_failure();
    bool exec(MYSQL_STMT* stmt, MYSQL_BIND* params);
    bool run_plain(const char* sql);

    ConnectionConfig cfg_{};
    MYSQL*           mysql_  = nullptr;
    bool             broken_ = true;

    MYSQL_STMT* st_save_           = nullptr;
    MYSQL_STMT* st_history_        = nullptr;
    MYSQL_STMT* st_counter_claim_  = nullptr;   // 멱등성 키 선점
    MYSQL_STMT* st_counter_bump_   = nullptr;   // 증가
    MYSQL_STMT* st_counter_read_   = nullptr;   // 현재 값

    // 프로시저 경로
    MYSQL_STMT* st_proc_save_    = nullptr;
    MYSQL_STMT* st_proc_history_ = nullptr;
    MYSQL_STMT* st_proc_counter_ = nullptr;

    unsigned    last_errno_ = 0;
    std::string last_error_;
};

} // namespace db
