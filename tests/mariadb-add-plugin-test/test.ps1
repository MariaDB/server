<#
.SYNOPSIS
  Builds the plugins in this directory against the given MariaDB package
  (an unpacked-ZIP layout, e.g. mariadb-X.Y.Z-winx64.zip) using nothing
  but find_package(mariadb-plugin) + MARIADB_ADD_PLUGIN(), the way an
  external plugin author would - then verifies mariadbd can actually
  load the resulting server plugins (MDEV-40608).

.PARAMETER Package
  Path to the MariaDB .zip package to test against.

.NOTES
  mariadbd's --plugin-load-add does not fail the process on a bad plugin -
  sql_plugin.cc discards plugin_load_list()'s return value at the call
  site in plugin_init(), so a failed load is only ever logged, never
  reflected in the exit code. This script therefore greps output for the
  failure message instead of trusting the exit code.
#>
param(
  [Parameter(Mandatory=$true, Position=0)]
  [string]$Package
)

$ErrorActionPreference = "Stop"

# Print, then run, an external command - like bash's `set -x`, but only
# for the commands this script actually runs (Set-PSDebug -Trace traces
# every line PowerShell itself executes, including its own internal
# error-formatting machinery, which is far noisier than we want here).
function Invoke-Traced {
  Write-Host "+ $($args -join ' ')"
  & $args[0] @($args[1..($args.Count-1)])
}

$Package = (Resolve-Path $Package).Path
$ScriptDir = $PSScriptRoot

$WorkDir = Join-Path ([System.IO.Path]::GetTempPath()) ("mariadb-add-plugin-test-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
Write-Host "== work dir: $WorkDir"

try {
  # Unpack into a fresh temp dir unrelated to wherever the archive was
  # built/downloaded, so the test can't accidentally depend on that
  # location - only on paths computed from the unpacked tree itself
  # (MDEV-40608's whole point).
  $UnpackedDir = Join-Path $WorkDir "unpacked"
  Write-Host "+ Expand-Archive -Path $Package -DestinationPath $UnpackedDir"
  Expand-Archive -Path $Package -DestinationPath $UnpackedDir

  $InstallRoot = (Get-ChildItem $UnpackedDir -Directory | Select-Object -First 1).FullName
  if (-not $InstallRoot) {
    throw "unpacked archive has no top-level directory"
  }
  Write-Host "== install root: $InstallRoot"

  $BuildDir = Join-Path $WorkDir "build"
  New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null

  Invoke-Traced cmake -S $ScriptDir -B $BuildDir -DCMAKE_PREFIX_PATH="$InstallRoot"
  if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
  Invoke-Traced cmake --build $BuildDir --config RelWithDebInfo
  if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }
  Invoke-Traced cmake --install $BuildDir --config RelWithDebInfo
  if ($LASTEXITCODE -ne 0) { throw "cmake install failed" }

  $Mariadbd = Join-Path $InstallRoot "bin\mariadbd.exe"
  $PluginDir = Join-Path $InstallRoot "lib\plugin"
  if (-not (Test-Path $Mariadbd)) {
    throw "$Mariadbd not found"
  }

  function Test-PluginLoads {
    param([string]$Lib, [string]$Expect)
    Write-Host "== checking $Lib loads into mariadbd"
    # mariadbd's own [Warning]/[ERROR] lines on stderr must not become
    # PowerShell terminating errors here - we need to inspect them
    # ourselves below, not have $ErrorActionPreference="Stop" throw on
    # the mere presence of stderr output from a 2>&1 merge.
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
      $out = Invoke-Traced $Mariadbd --no-defaults --plugin-dir="$PluginDir" `
        --plugin-load-add="$Lib" --help --verbose 2>&1 | Out-String
    } finally {
      $ErrorActionPreference = $prevEAP
    }
    if ($out -match "(?i)couldn't load plugin|\[ERROR\]") {
      Write-Host "FAIL: $Lib produced a load error:" -ForegroundColor Red
      $out -split "`n" | Select-String -Pattern "couldn't load plugin|\[ERROR\]" -CaseSensitive:$false
      throw "$Lib failed to load"
    }
    if ($out -notmatch [regex]::Escape($Expect)) {
      throw "$Lib loaded without a logged error, but its option ('$Expect') never appeared in --help output"
    }
    Write-Host "OK: $Lib"
  }

  Test-PluginLoads -Lib "dummy_auth.dll" -Expect "dummy-auth"
  Test-PluginLoads -Lib "ha_dummy_storage_engine.dll" -Expect "dummy-se"

  Write-Host "== dummy_client_auth.dll built and installed - client plugins only"
  Write-Host "   load during a connection handshake, not checked by this script"
  if (-not (Test-Path (Join-Path $PluginDir "dummy_client_auth.dll"))) {
    throw "dummy_client_auth.dll not installed"
  }

  Write-Host "ALL OK" -ForegroundColor Green
}
finally {
  Remove-Item $WorkDir -Recurse -Force -ErrorAction SilentlyContinue
}
