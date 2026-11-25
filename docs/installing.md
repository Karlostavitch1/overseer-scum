# Installing on a SCUM dedicated server

> Pre-alpha: until the engine bridge lands, `#` admin commands return
> "not implemented". Overseer commands and plugins work.

## Requirements
- Windows x64 SCUM dedicated server with file-system access (VPS/dedicated;
  many slot hosts don't allow adding DLLs)
- [.NET 8 Runtime (x64)](https://dotnet.microsoft.com/download/dotnet/8.0) —
  or ship an app-local runtime in `overseer/dotnet/`

## Install
1. Stop the server.
2. Build or download a release, which contains:
   ```
   version.dll
   overseer/
   ```
3. Copy both into `SCUM/Binaries/Win64/` next to `SCUMServer.exe`.
4. Start the server. On first start Overseer writes
   `overseer/config/overseer.json` with a random RCON password.
5. Check `overseer/logs/native.log` for `Overseer.Host initialized`.

To build a release layout yourself:
```powershell
./scripts/package.ps1            # outputs dist/
```

## Configure
`overseer/config/overseer.json`:
```json
{
  "rcon": {
    "enabled": true,
    "bindAddress": "127.0.0.1",
    "port": 27020,
    "password": "generated-on-first-run",
    "maxConnections": 8
  }
}
```
Restart the server after changing it.

## Uninstall / disable
Delete `version.dll` (and the `overseer` folder), or set the environment
variable `OVERSEER_DISABLE=1` for the server process.

## Plugins
Copy plugin DLLs into `overseer/plugins/` (or `overseer/plugins/<Name>/<Name>.dll`
if the plugin has dependencies), then run `overseer.reload` over RCON.
Plugins run with full server privileges — only install ones you trust.
