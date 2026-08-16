#include "mini_check.h"

#include <cstdio>

namespace mini {
namespace {
int g_failures = 0;
} // namespace

std::vector<Case>& registry() {
    // 정적 초기화 순서에 의존하지 않도록 함수 지역 정적으로 둔다
    static std::vector<Case> cases;
    return cases;
}

void report_failure(const char* file, int line, const char* expr) {
    ++g_failures;
    std::fprintf(stderr, "    FAIL  %s:%d  %s\n", file, line, expr);
}

int run_all() {
    int passed = 0;
    for (const auto& c : registry()) {
        const int before = g_failures;
        std::printf("[ RUN  ] %s\n", c.name);
        c.fn();
        if (g_failures == before) {
            ++passed;
            std::printf("[  OK  ] %s\n", c.name);
        } else {
            std::printf("[ FAIL ] %s\n", c.name);
        }
    }

    std::printf("\n%d/%zu cases passed, %d assertion failures\n",
                passed, registry().size(), g_failures);
    return g_failures == 0 ? 0 : 1;
}

} // namespace mini

int main() {
    return mini::run_all();
}
