//
//  설정 파서 검증.
//
//  설정이 잘못 읽히면 기본값으로 조용히 떨어진다.
//  그러면 "접속이 안 된다" 까지만 보이고 원인은 드러나지 않는다.
//
#include <cstdio>
#include <fstream>
#include <string>

#include "core/ini_reader.h"
#include "mini_check.h"

namespace {

std::string write_temp(const std::string& name, const std::string& content) {
    const std::string path = "test_" + name + ".ini";
    std::ofstream out(path, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return path;
}

} // namespace

TEST_CASE("구역과 키를 읽는다") {
    const auto path = write_temp("basic",
        "[mysql]\n"
        "host = 10.0.0.1\n"
        "port = 3307\n"
        "\n"
        "[server]\n"
        "listen_port = 9100\n");

    core::IniReader ini;
    CHECK(ini.load(path));
    CHECK(ini.get("mysql", "host") == "10.0.0.1");
    CHECK_EQ(ini.get_int("mysql", "port", 0), 3307);
    CHECK_EQ(ini.get_int("server", "listen_port", 0), 9100);

    std::remove(path.c_str());
}

TEST_CASE("UTF-8 BOM 이 있어도 첫 구역을 읽는다") {
    // 편집기로 저장하면 흔히 붙는다. 이걸 놓치면 설정 전체가 기본값이 된다
    const auto path = write_temp("bom",
        "\xEF\xBB\xBF[mysql]\n"
        "user = echo\n");

    core::IniReader ini;
    CHECK(ini.load(path));
    CHECK(ini.get("mysql", "user") == "echo");

    std::remove(path.c_str());
}

TEST_CASE("주석과 공백을 건너뛴다") {
    const auto path = write_temp("comment",
        "# 주석\n"
        "; 다른 주석\n"
        "  [mysql]  \n"
        "   user   =   echo   \n"
        "빈줄아래\n"
        "\n");

    core::IniReader ini;
    CHECK(ini.load(path));
    CHECK(ini.get("mysql", "user") == "echo");
    CHECK(!ini.has("mysql", "빈줄아래"));   // '=' 가 없는 줄은 무시한다

    std::remove(path.c_str());
}

TEST_CASE("값에 = 가 들어가도 첫 번째만 구분자로 본다") {
    const auto path = write_temp("eq",
        "[mysql]\n"
        "password = a=b=c\n");

    core::IniReader ini;
    CHECK(ini.load(path));
    CHECK(ini.get("mysql", "password") == "a=b=c");

    std::remove(path.c_str());
}

TEST_CASE("없는 키는 기본값을 돌려준다") {
    core::IniReader ini;
    CHECK(!ini.load("존재하지_않는_파일.ini"));
    CHECK(!ini.error().empty());
    CHECK(ini.get("mysql", "host", "127.0.0.1") == "127.0.0.1");
    CHECK_EQ(ini.get_int("mysql", "port", 3306), 3306);
}

TEST_CASE("숫자가 아닌 값은 기본값으로 떨어진다") {
    const auto path = write_temp("badnum",
        "[mysql]\n"
        "port = abc\n");

    core::IniReader ini;
    CHECK(ini.load(path));
    CHECK_EQ(ini.get_int("mysql", "port", 3306), 3306);

    std::remove(path.c_str());
}
