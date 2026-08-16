#include "core/log.h"

#include <share.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "core/swap_queue.h"

namespace core {
namespace {

// 한 줄의 상한. 넘치면 잘라 쓴다.
// 고정 크기라 항목마다 할당이 생기지 않는다
constexpr int kLineMax  = 240;
constexpr std::size_t kQueueCap = 65536;

struct Record {
    char line[kLineMax];
    int  length = 0;
};

LogLevel                     g_level = LogLevel::Info;
std::atomic<bool>            g_running{false};
std::atomic<unsigned long long> g_dropped{0};
SwapQueue<Record>            g_queue;
std::thread                  g_writer;
std::FILE*                   g_file = nullptr;

// 파일 포인터를 지킨다.
//
// 평소에는 기록 스레드만 파일을 만진다. 그런데 종료 플래그가 내려간 뒤에는
// 다른 스레드의 log_write 가 동기 경로로 들어와 같은 파일에 쓴다.
// 그 쓰기와 log_stop 의 fclose 가 겹칠 수 있다 - 창은 좁지만 종료마다 존재한다.
//
// 기록 스레드는 배치 하나마다 한 번만 잡는다. 항목마다 잡지 않으므로
// 흔한 경로의 비용은 5ms 에 한 번이다
std::mutex                   g_file_mutex;

// 회전 상태. 기록 스레드만 읽고 쓴다. 그래서 잠금이 없다
std::string                  g_path;
long long                    g_max_bytes = 0;
int                          g_keep      = 0;
long long                    g_written   = 0;   // 지금 파일에 쓴 양
int                          g_day       = 0;   // 파일을 연 날짜(yyyymmdd)

const char* level_tag(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

const char* base_name(const char* path) {
    const char* p = path;
    for (const char* c = path; *c; ++c) {
        if (*c == '/' || *c == '\\') p = c + 1;
    }
    return p;
}

// 호출부가 g_file_mutex 를 쥔 상태로 부른다
void emit(const Record& r) {
    std::fwrite(r.line, 1, static_cast<std::size_t>(r.length), stderr);
    if (g_file) {
        std::fwrite(r.line, 1, static_cast<std::size_t>(r.length), g_file);
        // 크기를 매번 물어보면 그만큼 시스템 호출이 는다. 쓴 만큼 더해 둔다
        g_written += r.length;
    }
}

// -------------------------------------------------------------------
//  회전 - 전부 기록 스레드에서만 호출된다
// -------------------------------------------------------------------
void local_time(std::tm& out) {
    const auto secs =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    ::localtime_s(&out, &secs);
}

int today_number() {
    std::tm tm{};
    local_time(tm);
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

void open_file() {
    // 공유 읽기를 허용해서 연다. 기본 열기로는 서버가 도는 동안
    // 로그를 열어볼 수 없어 장애 상황에서 쓸모가 없다
    g_file    = ::_fsopen(g_path.c_str(), "ab", _SH_DENYWR);
    g_day     = today_number();
    g_written = 0;

    // 이어 쓰는 경우가 있다. 지금 크기에서 세기 시작해야
    // 재기동을 반복할 때 회전이 밀리지 않는다
    if (g_file != nullptr) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(g_path, ec);
        if (!ec) g_written = static_cast<long long>(size);
    }
}

// 넘길 이름을 정한다. 같은 초에 두 번 넘기면 뒤에 번호를 붙인다
std::filesystem::path rotated_target() {
    namespace fs = std::filesystem;

    const fs::path    active(g_path);
    const fs::path    dir  = active.parent_path();
    const std::string ext  = active.extension().string();

    std::tm tm{};
    local_time(tm);
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);

    const std::string base = active.stem().string() + "." + stamp;

    std::error_code ec;
    for (int seq = 0; seq < 100; ++seq) {
        std::string name = base;
        if (seq > 0) name += "-" + std::to_string(seq);
        name += ext;

        fs::path candidate = dir.empty() ? fs::path(name) : dir / name;
        if (!fs::exists(candidate, ec)) return candidate;
    }
    return {};
}

// 보관 개수를 넘긴 것부터 지운다
void purge_old() {
    if (g_keep <= 0) return;

    namespace fs = std::filesystem;
    std::error_code ec;

    const fs::path active(g_path);
    fs::path       dir = active.parent_path();
    if (dir.empty()) dir = fs::path(".");

    const std::string prefix = active.stem().string() + ".";
    const std::string ext    = active.extension().string();

    std::vector<fs::path> rotated;
    fs::directory_iterator it(dir, ec);
    if (ec) return;

    for (const auto& entry : it) {
        const fs::path& p = entry.path();
        if (p.filename() == active.filename()) continue;
        if (p.extension().string() != ext) continue;
        if (p.filename().string().rfind(prefix, 0) != 0) continue;
        if (!fs::is_regular_file(p, ec)) continue;
        rotated.push_back(p);
    }

    if (static_cast<int>(rotated.size()) <= g_keep) return;

    // 이름에 시각이 들어 있으므로 이름순이 곧 시간순이다
    std::sort(rotated.begin(), rotated.end());
    const std::size_t excess = rotated.size() - static_cast<std::size_t>(g_keep);
    for (std::size_t i = 0; i < excess; ++i) fs::remove(rotated[i], ec);
}

void rotate_if_needed() {
    if (g_file == nullptr || g_path.empty()) return;

    const bool by_size = (g_max_bytes > 0 && g_written >= g_max_bytes);
    const bool by_day  = (g_day != 0 && today_number() != g_day);
    if (!by_size && !by_day) return;

    const std::filesystem::path target = rotated_target();
    if (target.empty()) return;

    std::fclose(g_file);
    g_file = nullptr;

    std::error_code ec;
    std::filesystem::rename(g_path, target, ec);

    // 넘기지 못했더라도 파일은 다시 연다.
    // 로그를 못 넘긴 것 때문에 기록 자체를 멈추지는 않는다
    open_file();
    if (!ec) purge_old();
}

void drain() {
    auto& batch = g_queue.swap();
    if (batch.empty()) return;

    // 배치 하나에 한 번만 잡는다. 회전도 파일을 여닫으므로 같은 구간 안에 둔다
    std::lock_guard<std::mutex> guard(g_file_mutex);

    for (const auto& r : batch) emit(r);
    batch.clear();

    std::fflush(stderr);
    if (g_file) std::fflush(g_file);

    rotate_if_needed();
}

void writer_loop() {
    while (g_running.load(std::memory_order_acquire)) {
        drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    drain();   // 종료 직전 남은 항목
}

} // namespace

bool log_start(const char* path, LogLevel level, long long max_bytes, int keep_files) {
    if (g_running.load(std::memory_order_acquire)) return true;

    g_level     = level;
    g_path      = (path != nullptr) ? path : "";
    g_max_bytes = max_bytes > 0 ? max_bytes : 0;
    g_keep      = keep_files > 0 ? keep_files : 0;

    // 기동 전에도 다른 스레드가 동기 경로로 들어와 있을 수 있다
    if (!g_path.empty()) {
        std::lock_guard<std::mutex> guard(g_file_mutex);
        open_file();
    }

    g_queue.reserve(1024);
    g_running.store(true, std::memory_order_release);
    g_writer = std::thread(writer_loop);
    return true;
}

void log_stop() {
    if (!g_running.exchange(false, std::memory_order_acq_rel)) return;
    if (g_writer.joinable()) g_writer.join();

    // 기록 스레드는 멈췄지만 다른 스레드가 동기 경로로 들어와 있을 수 있다
    std::lock_guard<std::mutex> guard(g_file_mutex);
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
}

void set_log_level(LogLevel level) { g_level = level; }

unsigned long long log_dropped() {
    return g_dropped.load(std::memory_order_relaxed);
}

void log_write(LogLevel level, const char* file, int line, const char* fmt, ...) {
    if (level < g_level) return;

    using namespace std::chrono;
    const auto now  = system_clock::now();
    const auto secs = system_clock::to_time_t(now);
    const auto ms   = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;

    std::tm tm{};
    ::localtime_s(&tm, &secs);

    char body[kLineMax];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);

    const auto tid =
        std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFF;

    Record r;
    // 조립은 호출 스레드에서 끝낸다. 큐에는 완성된 문자열만 실린다
    int n = std::snprintf(r.line, sizeof(r.line),
                          "%02d:%02d:%02d.%03d [%s] t%04llx %s (%s:%d)\n",
                          tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms),
                          level_tag(level),
                          static_cast<unsigned long long>(tid),
                          body, base_name(file), line);

    if (n < 0) return;
    if (n >= static_cast<int>(sizeof(r.line))) {
        n = static_cast<int>(sizeof(r.line)) - 1;
        r.line[n - 1] = '\n';
    }
    r.length = n;

    if (!g_running.load(std::memory_order_acquire)) {
        // 기동 전후 구간은 동기로 내보낸다. 여기서 잃으면 원인 추적이 끊긴다.
        // 종료 중이면 log_stop 이 같은 파일을 닫고 있을 수 있어 잠금을 잡는다
        std::lock_guard<std::mutex> guard(g_file_mutex);
        emit(r);
        std::fflush(stderr);
        return;
    }

    if (g_queue.pending() >= kQueueCap) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_queue.push(std::move(r));
}

} // namespace core
