-- =============================================================
--  iocp-echo-server : 스키마
--  대상 : MySQL 8.4 / InnoDB / utf8mb4
--  적용 : mysql -h 127.0.0.1 -u echo -p echodb < sql/001_schema.sql
-- =============================================================

SET NAMES utf8mb4;

-- -------------------------------------------------------------
--  echo_log : Save / History 메시지가 사용하는 표
--    - Save    : 1행 INSERT
--    - History : 세션의 최근 N행 조회
-- -------------------------------------------------------------
CREATE TABLE IF NOT EXISTS echo_log (
  log_id      BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  session_key CHAR(36)        NOT NULL,
  payload     VARCHAR(512)    NOT NULL,
  created_at  DATETIME(3)     NOT NULL DEFAULT CURRENT_TIMESTAMP(3),

  PRIMARY KEY (log_id),
  -- History 는 세션별 최신순 조회다. 정렬을 인덱스로 흡수한다
  KEY idx_session_recent (session_key, log_id DESC)
) ENGINE=InnoDB
  DEFAULT CHARSET=utf8mb4
  COLLATE=utf8mb4_0900_ai_ci
  COMMENT='에코 메시지 적재 · 조회 대상';

-- -------------------------------------------------------------
--  echo_counter : Counter 메시지의 갱신 대상
--    version 으로 낙관적 동시성 제어를 건다
-- -------------------------------------------------------------
CREATE TABLE IF NOT EXISTS echo_counter (
  session_key CHAR(36)        NOT NULL,
  hit_count   BIGINT UNSIGNED NOT NULL DEFAULT 0,
  version     INT UNSIGNED    NOT NULL DEFAULT 0,
  updated_at  DATETIME(3)     NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
                              ON UPDATE CURRENT_TIMESTAMP(3),

  PRIMARY KEY (session_key)
) ENGINE=InnoDB
  DEFAULT CHARSET=utf8mb4
  COLLATE=utf8mb4_0900_ai_ci
  COMMENT='세션별 카운터. 낙관적 동시성 제어 대상';

-- -------------------------------------------------------------
--  counter_request : 중복 요청 차단
--    클라이언트가 만든 request_key 를 기본 키로 둔다.
--    같은 키가 두 번 오면 INSERT 가 실패하고 트랜잭션이 되돌아간다.
--    서버 로직이 뚫려도 여기서 최종 판정된다
-- -------------------------------------------------------------
CREATE TABLE IF NOT EXISTS counter_request (
  request_key CHAR(36)    NOT NULL,
  session_key CHAR(36)    NOT NULL,
  applied_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),

  PRIMARY KEY (request_key),
  KEY idx_session (session_key, applied_at)
) ENGINE=InnoDB
  DEFAULT CHARSET=utf8mb4
  COLLATE=utf8mb4_0900_ai_ci
  COMMENT='멱등성 키 보관. 중복 요청 차단';
