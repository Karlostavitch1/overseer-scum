#pragma once

#include <filesystem>
#include <string_view>

namespace overseer::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// Opens (appends to) the log file. Safe to call once; later calls are ignored.
void Open(const std::filesystem::path& file);
void Write(Level level, std::string_view message);
void Close();

inline void Debug(std::string_view m) { Write(Level::Debug, m); }
inline void Info(std::string_view m) { Write(Level::Info, m); }
inline void Warn(std::string_view m) { Write(Level::Warn, m); }
inline void Error(std::string_view m) { Write(Level::Error, m); }

}  // namespace overseer::log
