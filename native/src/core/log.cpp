#include "core/log.h"

#include <windows.h>

#include <share.h>

#include <cstdio>
#include <mutex>
#include <string>

namespace overseer::log {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;

const char* LevelName(Level level) {
    switch (level) {
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO ";
        case Level::Warn: return "WARN ";
        case Level::Error: return "ERROR";
    }
    return "?????";
}

}  // namespace

void Open(const std::filesystem::path& file) {
    std::lock_guard lock(g_mutex);
    if (g_file) return;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    // Binary append: messages are already UTF-8.
    g_file = _wfsopen(file.c_str(), L"ab", _SH_DENYWR);
}

void Write(Level level, std::string_view message) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] ", t.wYear, t.wMonth,
             t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, LevelName(level));

    std::lock_guard lock(g_mutex);
    if (g_file) {
        fputs(prefix, g_file);
        fwrite(message.data(), 1, message.size(), g_file);
        fputc('\n', g_file);
        fflush(g_file);
    }
    std::string line = std::string(prefix) + std::string(message) + "\n";
    OutputDebugStringA(line.c_str());
}

void Close() {
    std::lock_guard lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

}  // namespace overseer::log
