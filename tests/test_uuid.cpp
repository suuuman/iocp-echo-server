//
//  세션 키 생성 검증.
//
//  형식이 틀려도 서버는 그대로 돌아가고 DB 도 CHAR(36) 이라 받아준다.
//  그래서 눈으로는 늦게까지 드러나지 않는다. 여기서 고정한다.
//
#include <string>
#include <unordered_set>

#include "core/uuid.h"
#include "mini_check.h"

namespace {

bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

} // namespace

TEST_CASE("길이가 36자다") {
    CHECK_EQ(core::make_session_key().size(), static_cast<std::size_t>(36));
}

TEST_CASE("구분자가 8 13 18 23 위치에만 있다") {
    const std::string key = core::make_session_key();

    for (std::size_t i = 0; i < key.size(); ++i) {
        const bool separator_slot = (i == 8 || i == 13 || i == 18 || i == 23);
        if (separator_slot) {
            CHECK_EQ(key[i], '-');
        } else {
            // 채우지 못한 자리가 '-' 로 남아 있으면 여기서 걸린다
            CHECK(is_hex(key[i]));
        }
    }
}

TEST_CASE("버전과 변형 자리가 규격을 따른다") {
    const std::string key = core::make_session_key();
    CHECK_EQ(key[14], '4');                    // 버전 4
    const char v = key[19];                    // 변형 - 8 9 a b 중 하나
    CHECK(v == '8' || v == '9' || v == 'a' || v == 'b');
}

TEST_CASE("반복 생성해도 겹치지 않는다") {
    constexpr int kCount = 20000;

    std::unordered_set<std::string> seen;
    seen.reserve(kCount);
    for (int i = 0; i < kCount; ++i) seen.insert(core::make_session_key());

    CHECK_EQ(seen.size(), static_cast<std::size_t>(kCount));
}
