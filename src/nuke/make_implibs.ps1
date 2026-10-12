# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# Generate import libraries for Nuke DLLs that ship without one (Python, Boost.Python).
# Usage: pwsh make_implibs.ps1 <NUKE_ROOT> <OUT_DIR>
param([Parameter(Mandatory)] [string]$NukeRoot, [Parameter(Mandatory)] [string]$OutDir)
$ErrorActionPreference = 'Stop'
$msvc = Get-ChildItem 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC' |
  Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$dlls = Get-ChildItem $NukeRoot -Filter *.dll | Where-Object { $_.Name -match '^(python3\d+|foundryboost_python3\d+.*)\.dll$' }
foreach ($dll in $dlls) {
  $name = $dll.BaseName
  $exports = & "$bin\dumpbin.exe" /nologo /exports $dll.FullName
  # Export rows: ordinal, hint, RVA, name.
  $names = $exports | ForEach-Object { if ($_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)') { $Matches[1] } }
  $def = Join-Path $OutDir "$name.def"
  @("LIBRARY $name", 'EXPORTS') + ($names | ForEach-Object { "  $_" }) | Set-Content -Encoding ascii $def
  & "$bin\lib.exe" /nologo /machine:x64 "/def:$def" "/out:$(Join-Path $OutDir "$name.lib")"
  if ($LASTEXITCODE) { throw "lib.exe failed for $name" }
  "$name : $($names.Count) exports"
}
