#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

// Minimal key=value parser for simple config files.
class KvConfig {
public:
    // Load and parse; returns true on success.
    bool load(const std::string &path);

    // Get raw value; empty optional if missing.
    std::optional<std::string> get(const std::string &key) const;

    // Convenience getters.
    std::optional<int> get_int(const std::string &key) const;

    const std::unordered_map<std::string, std::string> &all() const { return kv_; }

private:
    std::unordered_map<std::string, std::string> kv_;
};
