-- =============================================================
--  iocp-echo-server : 저장 프로시저
--  적용 : mysql -h 127.0.0.1 -u echo -p echodb < sql/002_procedures.sql
--
--  같은 일을 하는 경로가 두 가지 있다.
--    직접 SQL  - 애플리케이션이 구문을 나눠 보내고 트랜잭션도 직접 연다
--    프로시저  - 한 번의 호출로 끝난다
--  둘 다 남겨 두고 설정(use_procedures)으로 고른다. 비교 측정을 위해서다.
-- =============================================================

DROP PROCEDURE IF EXISTS sp_save_log;
DROP PROCEDURE IF EXISTS sp_get_history;
DROP PROCEDURE IF EXISTS sp_apply_counter;

DELIMITER $$

-- -------------------------------------------------------------
--  sp_save_log
--    적재 후 생성된 식별자를 OUT 으로 돌려준다.
--    직접 SQL 경로에서는 INSERT 후 insert_id 를 별도로 읽어야 한다
-- -------------------------------------------------------------
CREATE PROCEDURE sp_save_log(
    IN  p_session_key CHAR(36),
    IN  p_payload     VARCHAR(512),
    OUT p_log_id      BIGINT UNSIGNED
)
BEGIN
    INSERT INTO echo_log(session_key, payload) VALUES(p_session_key, p_payload);
    SET p_log_id = LAST_INSERT_ID();
END $$

-- -------------------------------------------------------------
--  sp_get_history
--    결과셋을 그대로 반환한다. 호출부는 행을 받아 매핑만 한다
-- -------------------------------------------------------------
CREATE PROCEDURE sp_get_history(
    IN p_session_key CHAR(36),
    IN p_limit       INT UNSIGNED
)
BEGIN
    SELECT log_id,
           payload,
           CAST(UNIX_TIMESTAMP(created_at) * 1000 AS UNSIGNED) AS created_at_ms
      FROM echo_log
     WHERE session_key = p_session_key
     ORDER BY log_id DESC
     LIMIT p_limit;
END $$

-- -------------------------------------------------------------
--  sp_apply_counter
--
--  직접 SQL 경로는 다음 다섯 번을 왕복한다.
--    START TRANSACTION → 멱등성 키 선점 → 증가 → 값 조회 → COMMIT
--  여기서는 한 번의 호출로 끝난다.
--
--  중복 키는 오류가 아니라 "이미 반영됨" 판정이다.
--  응답을 받지 못한 클라이언트가 재시도하는 것은 옳고,
--  서버는 그 재시도에 같은 결과를 돌려주어야 한다.
-- -------------------------------------------------------------
CREATE PROCEDURE sp_apply_counter(
    IN  p_request_key     CHAR(36),
    IN  p_session_key     CHAR(36),
    OUT p_hit_count       BIGINT UNSIGNED,
    OUT p_already_applied TINYINT
)
BEGIN
    DECLARE v_duplicate TINYINT DEFAULT 0;

    -- 중복 키만 따로 받는다. 그 밖의 오류는 되돌리고 그대로 올린다
    DECLARE CONTINUE HANDLER FOR 1062 SET v_duplicate = 1;
    DECLARE EXIT HANDLER FOR SQLEXCEPTION
    BEGIN
        ROLLBACK;
        RESIGNAL;
    END;

    SET p_already_applied = 0;

    START TRANSACTION;

    -- 멱등성 키 선점. 이미 있으면 핸들러가 v_duplicate 를 세운다
    INSERT INTO counter_request(request_key, session_key)
    VALUES(p_request_key, p_session_key);

    IF v_duplicate = 1 THEN
        ROLLBACK;
        SET p_already_applied = 1;
        SELECT hit_count INTO p_hit_count
          FROM echo_counter WHERE session_key = p_session_key;
        SET p_hit_count = IFNULL(p_hit_count, 0);
    ELSE
        INSERT INTO echo_counter(session_key, hit_count, version)
        VALUES(p_session_key, 1, 1)
        ON DUPLICATE KEY UPDATE hit_count = hit_count + 1,
                                version   = version + 1;

        SELECT hit_count INTO p_hit_count
          FROM echo_counter WHERE session_key = p_session_key;

        COMMIT;
    END IF;
END $$

DELIMITER ;
