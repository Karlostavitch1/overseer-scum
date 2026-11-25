#pragma once

#include <filesystem>

namespace overseer::ue4 {

// If reflection probing is enabled (env OVERSEER_REFLECTION_DUMP=1 or the
// file <root>/config/dump-reflection exists), starts a background thread that
// waits for the engine to finish loading, locates the reflection tables and
// writes <root>/logs/reflection-dump.txt. Diagnostic only; never commit dumps.
void StartProbeIfEnabled(const std::filesystem::path& root);

}  // namespace overseer::ue4
