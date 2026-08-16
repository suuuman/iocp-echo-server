#pragma once
//
//  최소 ini 파서.
//
//  설정 형식에 라이브러리를 붙일 이유가 없다. 필요한 것은
//  [구역] 과 키 = 값 두 가지뿐이다.
//
#include <map>
#include <string>

namespace core {

class IniReader {
public:
    bool load(const std::string& path);

    std::string get(const std::string& section, const std::string& key,
                    const std::string& fallback = {}) const;
    int  get_int(const std::string& section, const std::string& key, int fallback) const;

    bool has(const std::string& section, const std::string& key) const;
    const std::string& error() const noexcept { return error_; }

private:
    static std::string trim(const std::string& s);

    std::map<std::string, std::string> values_;   // "구역.키" -> 값
    std::string                        error_;
};

} // namespace core
