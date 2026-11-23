# Roadmap

## Milestone 0 — Foundations ✅ (this scaffold)
- [x] `version.dll` proxy loader and native bootstrap
- [x] .NET 8 hosting via hostfxr, versioned native/managed ABI
- [x] Source RCON server + client, with tests
- [x] Command router, built-in `overseer.*` commands
- [x] Plugin API, collectible plugin loading, hot reload
- [x] Dev host with fake engine, RCON CLI, CI

## Milestone 1 — Engine bridge: run admin commands  ← **help wanted**
The single most important piece. Goal: `#ListPlayers` over RCON returns the
real output.

- [ ] Confirm `SCUMServer.exe` loads `version.dll` from its directory; add
      alternative proxies if not.
- [ ] Choose the reflection layer: build on [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS)
      (MIT) for `GObjects`/`GNames`/`ProcessEvent` access, or a minimal
      in-house pattern scanner. Prefer reuse.
- [ ] Locate SCUM's admin command dispatcher (the code path that handles `#`
      chat commands from an admin) and the object it needs.
- [ ] Get onto the game thread (e.g. hook a per-tick function) and drain a
      command queue there; capture output.
- [ ] Signature/pattern-based lookups so minor game updates don't break it;
      fail safe to `NullEngineBridge` if a signature is missing.
- [ ] Document findings in `docs/engine-notes/` (methods, not copied dumps).

## Milestone 2 — Events
- [ ] Player connected / disconnected, chat message, kill, admin command used
- [ ] `OverseerPlugin` virtual hooks + RCON event stream (opt-in)
- [ ] Alternative source while bridge hooks mature: tail SCUM's own log files
      (`SCUM/Saved/SaveFiles/Logs`) for chat, login, kill and admin events

## Milestone 3 — Plugin developer experience
- [ ] Publish `Overseer.Api` to NuGet; `dotnet new overseer-plugin` template
- [ ] Per-plugin JSON config helper, timers, persistent data helpers
- [ ] Optional: compile `.cs` drop-in plugins with Roslyn
- [ ] Read-only access to `SCUM.db` (SQLite) via a helper

## Milestone 4 — In-game commands & permissions
- [ ] Chat command handling (`/cmd`), permission groups, Steam ID mapping
- [ ] Enforce `CommandAttribute.Permission`

## Milestone 5 — Operations
- [ ] Optional WebSocket/HTTP API with token auth (for web panels)
- [ ] Release packaging with app-local .NET runtime option
- [ ] Health/metrics command, structured logs
