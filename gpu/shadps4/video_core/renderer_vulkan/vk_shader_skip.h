// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: GOW3_SKIP_FILE=<path>: draws and dispatches whose vertex, pixel or compute shader hash
// (the 8 hex digits of the frame analyzer's "vs/ps" and "cs" columns) is listed in the file are
// not recorded. The file is read again whenever it changes, so a tester can bisect which pass
// makes an artifact while the game runs: hex hashes separated by spaces, commas or new lines,
// '#' starts a comment. Off (no cost) unless the variable is set.

#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>

namespace Vulkan::ShaderSkip {

namespace Detail {
struct State {
    std::mutex mutex;
    std::unordered_set<std::uint32_t> hashes;
    std::filesystem::file_time_type stamp{};
    std::chrono::steady_clock::time_point next_check{};
    bool exists{};
};

inline const char* Path() {
    static const char* path = [] {
        const char* p = std::getenv("GOW3_SKIP_FILE");
        if (p && *p) {
            std::printf("GPU: shader skip list from %s (GOW3_SKIP_FILE)\n", p);
            return p;
        }
        return static_cast<const char*>(nullptr);
    }();
    return path;
}

inline State& Get() {
    static State state;
    return state;
}

inline void Reload(State& s, const char* path) {
    s.hashes.clear();
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (const auto hash = line.find('#'); hash != std::string::npos) {
            line.resize(hash);
        }
        for (char& c : line) {
            if (c == ',' || c == ';' || c == '\t' || c == '\r') {
                c = ' ';
            }
        }
        std::size_t pos = 0;
        while (pos < line.size()) {
            const auto start = line.find_first_not_of(' ', pos);
            if (start == std::string::npos) {
                break;
            }
            const auto end = line.find(' ', start);
            const std::string token = line.substr(start, end - start);
            pos = end == std::string::npos ? line.size() : end;
            char* tail = nullptr;
            const unsigned long long value = std::strtoull(token.c_str(), &tail, 16);
            if (tail && *tail == '\0' && !token.empty()) {
                s.hashes.insert(static_cast<std::uint32_t>(value));
            }
        }
    }
    std::printf("GPU: shader skip list: %zu hashes\n", s.hashes.size());
}
} // namespace Detail

/// True when a draw or dispatch with these shader hashes must not be recorded.
inline bool Skip(std::uint64_t first, std::uint64_t second = 0) {
    const char* path = Detail::Path();
    if (!path) {
        return false;
    }
    auto& s = Detail::Get();
    std::scoped_lock lock{s.mutex};
    const auto now = std::chrono::steady_clock::now();
    if (now >= s.next_check) {
        s.next_check = now + std::chrono::milliseconds(500);
        std::error_code ec;
        const auto stamp = std::filesystem::last_write_time(path, ec);
        if (ec) {
            if (s.exists) {
                s.exists = false;
                s.hashes.clear();
                std::printf("GPU: shader skip list: file gone, nothing skipped\n");
            }
        } else if (!s.exists || stamp != s.stamp) {
            s.exists = true;
            s.stamp = stamp;
            Detail::Reload(s, path);
        }
    }
    if (s.hashes.empty()) {
        return false;
    }
    return (first && s.hashes.contains(static_cast<std::uint32_t>(first))) ||
           (second && s.hashes.contains(static_cast<std::uint32_t>(second)));
}

} // namespace Vulkan::ShaderSkip
