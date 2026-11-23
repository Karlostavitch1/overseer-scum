# Contributing to Overseer

Thanks for helping. Overseer is early, so there is a lot of room to shape it.

## Ground rules

1. **Clean-room only.** Do not copy code, decompiled output or assets from
   Oxygen, ggCON, or any other closed or differently-licensed SCUM tool. Do not
   commit SCUM game files, SDK dumps generated from game binaries, or offsets
   copied from other projects. Describe *how* you found something instead.
   See [docs/legal-and-fair-play.md](docs/legal-and-fair-play.md).
2. **Server-side only.** No features that run on, inspect, or modify game clients.
3. **Be kind.** We follow the [Code of Conduct](CODE_OF_CONDUCT.md).

## Getting set up

- .NET 8 SDK
- Visual Studio 2022 with "Desktop development with C++" (or Build Tools) and CMake 3.24+
- Optional: a SCUM dedicated server (installed via SteamCMD) for engine-bridge work

```bash
dotnet test managed/Overseer.sln
cmake -S native -B build/native -A x64 && cmake --build build/native --config Release
```

Most managed work can be done entirely against `Overseer.DevHost`, which runs
the runtime with a fake engine — no game server required.

## Where help is needed

Check issues labelled `good first issue` and `help wanted`, and the
[roadmap](docs/roadmap.md). The biggest open area is the **engine bridge**:
finding SCUM's admin command dispatcher and the game thread, and calling them
safely.

## Pull requests

- One logical change per PR; include tests for managed code.
- Run `dotnet format managed/Overseer.sln` before pushing.
- Public API changes in `Overseer.Api` need a note in the PR description —
  plugins depend on it, so we treat it as a stable contract.
- By contributing you agree your work is licensed under the MIT license.
