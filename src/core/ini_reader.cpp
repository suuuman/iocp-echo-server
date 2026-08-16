#include "core/ini_reader.h"

#include <cstdlib>
#include <fstream>

namespace core {

std::string IniReader::trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

bool IniReader::load(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        error_ = "설정 파일을 열 수 없다: " + path;
        return false;
    }

    std::string section;
    std::string line;
    bool        first_line = true;

    while (std::getline(in, line)) {
        // 편집기가 붙인 UTF-8 BOM 을 걷어낸다.
        // 남겨 두면 첫 구역 이름이 어긋나 설정 전체가 기본값으로 떨어진다
        if (first_line) {
            first_line = false;
            if (line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
        }

        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key   = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (key.empty()) continue;

        values_[section + "." + key] = value;
    }

    return true;
}

bool IniReader::has(const std::string& section, const std::string& key) const {
    return values_.find(section + "." + key) != values_.end();
}

std::string IniReader::get(const std::string& section, const std::string& key,
                           const std::string& fallback) const {
    const auto it = values_.find(section + "." + key);
    return it == values_.end() ? fallback : it->second;
}

int IniReader::get_int(const std::string& section, const std::string& key, int fallback) const {
    const auto it = values_.find(section + "." + key);
    if (it == values_.end()) return fallback;

    try {
        return std::stoi(it->second);
    } catch (...) {
        // 값이 숫자가 아니면 기본값을 쓴다. 여기서 죽일 이유가 없다
        return fallback;
    }
}

} // namespace core
