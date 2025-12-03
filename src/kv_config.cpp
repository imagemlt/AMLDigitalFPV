#include "kv_config.h"

#include <fstream>
#include <sstream>
#include <algorithm>

namespace {
inline std::string trim(const std::string &s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}
}

bool KvConfig::load(const std::string &path)
{
    kv_.clear();
    std::ifstream ifs(path);
    if (!ifs.is_open())
        return false;
    std::string line;
    while (std::getline(ifs, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (!key.empty())
            kv_[key] = val;
    }
    return true;
}

std::optional<std::string> KvConfig::get(const std::string &key) const
{
    auto it = kv_.find(key);
    if (it == kv_.end())
        return std::nullopt;
    return it->second;
}

std::optional<int> KvConfig::get_int(const std::string &key) const
{
    auto v = get(key);
    if (!v)
        return std::nullopt;
    try {
        return std::stoi(*v);
    } catch (...) {
        return std::nullopt;
    }
}
