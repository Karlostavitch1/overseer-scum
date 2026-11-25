<#
.SYNOPSIS
  Builds Overseer and lays it out as it is installed next to SCUMServer.exe.

.EXAMPLE
  ./scripts/package.ps1                 # -> dist/
  ./scripts/package.ps1 -Configuration Debug -Output out/test
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string] $Configuration = 'Release',
    [string] $Output = 'dist'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$out = Join-Path $repo $Output
$nativeBuild = Join-Path $repo 'build/native'

Write-Host "==> Native ($Configuration)"
cmake -S (Join-Path $repo 'native') -B $nativeBuild -A x64 -Wno-deprecated
if ($LASTEXITCODE) { throw 'cmake configure failed' }
cmake --build $nativeBuild --config $Configuration
if ($LASTEXITCODE) { throw 'cmake build failed' }

Write-Host '==> Managed'
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
dotnet publish (Join-Path $repo 'managed/src/Overseer.Host/Overseer.Host.csproj') `
    -c $Configuration -o (Join-Path $out 'overseer/managed')
if ($LASTEXITCODE) { throw 'dotnet publish failed' }

Write-Host '==> Layout'
New-Item -ItemType Directory -Force -Path (Join-Path $out 'overseer/bin'), (Join-Path $out 'overseer/plugins') | Out-Null
Copy-Item (Join-Path $nativeBuild "$Configuration/version.dll") $out
Copy-Item (Join-Path $nativeBuild "$Configuration/overseer_native.dll") (Join-Path $out 'overseer/bin')
Copy-Item (Join-Path $repo 'LICENSE'), (Join-Path $repo 'THIRD_PARTY_NOTICES.md') (Join-Path $out 'overseer')

Write-Host "Done. Copy the contents of '$out' into SCUM/Binaries/Win64/."
