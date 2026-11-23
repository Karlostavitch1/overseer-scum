# Security Policy

Overseer runs inside a game server process and exposes a network service, so
we take security reports seriously.

## Reporting a vulnerability

Please **do not** open a public issue. Use GitHub's
[private vulnerability reporting](../../security/advisories/new) for this
repository. We aim to acknowledge reports within 7 days.

## Scope

In scope: the RCON server, authentication, the plugin loader, native/managed
interop, and the packaging scripts.

## Operator guidance

- RCON binds to `127.0.0.1` by default. If you expose it, put it behind a
  firewall allow-list or VPN — the Source RCON protocol is **not encrypted**.
- Use a long random password (the default generated one is fine).
- Only install plugins you trust: plugins run with full server privileges.
