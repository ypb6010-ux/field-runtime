// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/log/Sinks.h"

#include <cstdio>
#include <ctime>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

#include "core/dp/Value.h"

namespace core::log {

namespace {

std::string pad(std::string s, std::size_t width) {
    if (s.size() < width) s.append(width - s.size(), ' ');
    return s;
}

// Local wall-clock "yyyy-MM-dd HH:mm:ss.zzz" — Qt-free, portable.
std::string tsString(LogTime ts) {
    auto const sinceEpoch = ts.time_since_epoch();
    auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(sinceEpoch).count() % 1000;
    std::time_t const tt = std::chrono::system_clock::to_time_t(ts);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, int(ms));
    return buf;
}

std::string categoryTag(std::string const& category) {
    std::string c = category.empty() ? std::string("app") : category;
    if (!c.empty() && c[0] >= 'a' && c[0] <= 'z') c[0] = char(c[0] - 'a' + 'A');
    return "[Core/" + c + "]";
}

std::string fieldsToString(std::map<std::string, dp::Value> const& fields) {
    if (fields.empty()) return {};
    std::string out;
    for (auto const& [key, value] : fields) {
        out += ' ';
        out += key;
        out += '=';
        out += dp::toString(value);
    }
    return out;
}

char const* ansiColour(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:    return "\033[90m";  // bright black
        case LogLevel::Debug:    return "\033[36m";  // cyan
        case LogLevel::Info:     return "\033[0m";   // default
        case LogLevel::Warn:     return "\033[33m";  // yellow
        case LogLevel::Error:    return "\033[31m";  // red
        case LogLevel::Critical: return "\033[1;31m";// bold red
    }
    return "\033[0m";
}

} // namespace

std::string formatLine(LogRecord const& rec) {
    std::string out = tsString(rec.ts);
    out += ' ';
    out += pad(levelName(rec.level), 5);
    out += ' ';
    out += pad(categoryTag(rec.category), 16);
    out += ' ';
    if (rec.source.empty()) {
        out += rec.message;
    } else {
        out += rec.source;
        out += ' ';
        out += rec.message;
    }
    out += fieldsToString(rec.fields);
    return out;
}

std::string formatLine(OperationRecord const& rec) {
    std::string out = tsString(rec.ts);
    out += " OP    [Core/Operation] action=";
    out += rec.action;
    out += " target=";
    out += rec.target;
    out += ' ';
    out += dp::toString(rec.oldValue);
    out += "->";
    out += dp::toString(rec.newValue);
    out += " result=";
    out += rec.result;
    if (!rec.note.empty()) {
        out += " note=";
        out += rec.note;
    }
    out += " actor=";
    out += rec.actor;
    return out;
}

// ---------------------------------------------------------------------------
// ConsoleSink
// ---------------------------------------------------------------------------
void ConsoleSink::write(LogRecord const& rec) {
    std::string const line = formatLine(rec);
    if (m_colour) {
        std::fprintf(stderr, "%s%s\033[0m\n", ansiColour(rec.level), line.c_str());
    } else {
        std::fprintf(stderr, "%s\n", line.c_str());
    }
}

void ConsoleSink::write(OperationRecord const& rec) {
    std::fprintf(stderr, "%s\n", formatLine(rec).c_str());
}

// ---------------------------------------------------------------------------
// RollingFileSink
// ---------------------------------------------------------------------------
class RollingFileSink::Impl {
public:
    Impl(std::string p, std::int64_t maxBytes, int maxFiles)
        : path(std::move(p)), maxBytes(maxBytes), maxFiles(maxFiles) {
        std::error_code ec;
        std::filesystem::path const fp(path);
        if (fp.has_parent_path()) std::filesystem::create_directories(fp.parent_path(), ec);
        open();
    }

    ~Impl() {
        std::lock_guard lk(mtx);
        if (file.is_open()) file.close();
    }

    void open() {
        file.open(path, std::ios::out | std::ios::app | std::ios::binary);
    }

    void rotateIfNeeded() {
        if (maxBytes <= 0) return;
        std::error_code ec;
        auto const sz = std::filesystem::file_size(path, ec);
        if (ec || std::int64_t(sz) < maxBytes) return;
        file.close();
        // path.<maxFiles> is dropped; shift the rest up by one.
        std::filesystem::remove(path + "." + std::to_string(maxFiles), ec);
        for (int i = maxFiles - 1; i >= 1; --i) {
            auto const from = path + "." + std::to_string(i);
            auto const to   = path + "." + std::to_string(i + 1);
            if (std::filesystem::exists(from, ec))
                std::filesystem::rename(from, to, ec);
        }
        std::filesystem::rename(path, path + ".1", ec);
        open();
    }

    void writeLine(std::string const& line) {
        std::lock_guard lk(mtx);
        if (!file.is_open()) return;
        file << line << '\n';
        file.flush();
        rotateIfNeeded();
    }

    void doFlush() {
        std::lock_guard lk(mtx);
        if (file.is_open()) file.flush();
    }

    std::string   path;
    std::int64_t  maxBytes;
    int           maxFiles;
    std::ofstream file;
    std::mutex    mtx;
};

RollingFileSink::RollingFileSink(std::string filePath, std::int64_t maxBytes, int maxFiles)
    : m_impl(std::make_unique<Impl>(std::move(filePath), maxBytes, maxFiles)) {}

RollingFileSink::~RollingFileSink() = default;

void RollingFileSink::write(LogRecord const& rec)       { m_impl->writeLine(formatLine(rec)); }
void RollingFileSink::write(OperationRecord const& rec) { m_impl->writeLine(formatLine(rec)); }
void RollingFileSink::flush()                           { m_impl->doFlush(); }

} // namespace core::log
