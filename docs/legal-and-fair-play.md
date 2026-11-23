# Legal & fair play

Overseer is an independent, community project. It is not affiliated with or
endorsed by Gamepires, Jagex, or the authors of Oxygen or ggCON.

## Scope
- **Server-side only.** Overseer loads into the dedicated server you operate.
  It does not run on, inspect, or modify game clients, and features that would
  give players an unfair advantage are out of scope.
- **Your server, your responsibility.** Check the SCUM server hosting rules and
  your host's terms before installing third-party DLLs.

## Clean-room policy
To keep the project safe to use and redistribute under MIT:

- Never commit code, decompiled output, binaries or assets from closed-source
  projects (including Oxygen and ggCON) or from SCUM itself.
- Don't commit raw SDK dumps or offset tables produced from game binaries.
  Commit the *method* (signatures, how a function was found) and generate any
  version-specific data at runtime.
- Behavioural compatibility (e.g. similar command names or a similar plugin
  style) is fine; copied implementation is not.
- Third-party open source code is welcome if its license is compatible with
  MIT — keep its license notice and list it in `THIRD_PARTY_NOTICES.md`.
