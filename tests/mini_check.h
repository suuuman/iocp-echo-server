#pragma once
//
//  최소 테스트 하네스.
//
//  외부 프레임워크를 두지 않은 이유 -
//    이 저장소는 clone 후 빌드까지 추가 다운로드가 없는 것을 원칙으로 한다.
//    검증에 필요한 것은 케이스 등록과 단언 두 가지뿐이라 직접 둔다.
//
#include <vector>

namespace mini {

using CaseFn = void (*)();

struct Case {
    const char* name;
    CaseFn      fn;
};

std::vector<Case>& registry();
void report_failure(const char* file, int line, const char* expr);
int  run_all();

struct Registrar {
    Registrar(const char* name, CaseFn fn) { registry().push_back(Case{name, fn}); }
};

} // namespace mini

#define MINI_CAT2(a, b) a##b
#define MINI_CAT(a, b)  MINI_CAT2(a, b)

#define TEST_CASE(name)                                                      \
    static void MINI_CAT(mini_case_, __LINE__)();                            \
    static ::mini::Registrar MINI_CAT(mini_reg_, __LINE__)(                  \
        name, &MINI_CAT(mini_case_, __LINE__));                              \
    static void MINI_CAT(mini_case_, __LINE__)()

#define CHECK(expr)                                                          \
    do {                                                                     \
        if (!(expr)) ::mini::report_failure(__FILE__, __LINE__, #expr);      \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))
