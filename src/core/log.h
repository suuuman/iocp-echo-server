#pragma once
//
//  비동기 로거.
//
//  기록 스레드를 따로 두어 파일 쓰기가 호출 스레드를 붙잡지 않게 한다.
//  로직 스레드가 파일 I/O 로 멈추면 그 순간 모든 처리가 함께 멈추기 때문이다.
//
//  문자열 조립은 호출 스레드에서 한다. 인자 수명을 큐에 실을 수 없어서다.
//  뒤로 넘기는 것은 조립 결과의 "쓰기" 뿐이다.
//
#include <cstdarg>

namespace core {

enum class LogLevel { Debug, Info, Warn, Error };

// path 가 비어 있으면 파일에 쓰지 않고 표준 오류로만 내보낸다.
//
// 회전 - 쓴 양이 max_bytes 를 넘거나 날짜가 바뀌면 시각을 붙인 이름으로 넘기고
//        새 파일을 연다. keep_files 는 넘긴 파일의 보관 개수다.
//        max_bytes 가 0 이면 회전하지 않고, keep_files 가 0 이면 지우지 않는다
bool log_start(const char* path, LogLevel level,
               long long max_bytes = 64ll * 1024 * 1024, int keep_files = 10);
void log_stop();   // 남은 항목을 모두 기록하고 종료한다

void set_log_level(LogLevel level);
void log_write(LogLevel level, const char* file, int line, const char* fmt, ...);

// 큐에 자리가 없어 버린 건수. 0 이어야 정상이다
unsigned long long log_dropped();

} // namespace core

#define LOG_DEBUG(...) ::core::log_write(::core::LogLevel::Debug, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  ::core::log_write(::core::LogLevel::Info,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  ::core::log_write(::core::LogLevel::Warn,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) ::core::log_write(::core::LogLevel::Error, __FILE__, __LINE__, __VA_ARGS__)
