## What and why

<!-- What does this change and why is it needed? Link issues with "Fixes #123". -->

## Checklist

- [ ] Tests added/updated (managed code) and `dotnet test managed/Overseer.sln` passes
- [ ] Native code builds (`cmake --build build/native --config Release`)
- [ ] `Overseer.Api` public API changes are described above (or there are none)
- [ ] Native/managed ABI changes update both `native_api.h` and `NativeApi.cs` and bump the ABI version (or there are none)
- [ ] Clean-room: no code, dumps or assets from closed-source tools or SCUM itself ([policy](../docs/legal-and-fair-play.md))
