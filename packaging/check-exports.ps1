# Usage: check-exports.ps1 -Dll <tblog.dll> [-Header <include/tblog/log.h>] [-Tool <dumpbin|objdump>]
#
# Windows counterpart of check-symbols.sh: fails unless tblog.dll exports
# exactly the functions <tblog/log.h> declares with TBLOG_API (a public
# function missing TBLOG_API is not dllexport'ed and shows up as "missing";
# an internal one that leaks shows up as "unexpected"). The export table is
# read with `dumpbin /exports` (MSVC) or `objdump -p` (MinGW-w64 binutils).
# Works with Windows PowerShell 5.1 and PowerShell 7. Registered as the
# `check_exports` CTest test on Windows.
param(
    [Parameter(Mandatory = $true)][string]$Dll,
    [string]$Header = "",
    [string]$Tool = ""
)
$ErrorActionPreference = "Stop"
# ($PSScriptRoot is not usable in param defaults on Windows PowerShell 5.1.)
if (-not $Header) {
    $Header = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "..\include\tblog\log.h"
}

if (-not $Tool) {
    foreach ($candidate in @("dumpbin", "objdump", "x86_64-w64-mingw32-objdump")) {
        if (Get-Command $candidate -ErrorAction SilentlyContinue) { $Tool = $candidate; break }
    }
    if (-not $Tool) { Write-Error "check-exports: neither dumpbin nor objdump found (pass -Tool)"; exit 2 }
}

# Expected: TBLOG_API functions of the public header.
$text = Get-Content -Raw -Path $Header
$expected = @([regex]::Matches($text, 'TBLOG_API\b[^;(]*?\b(tblog_\w+)\s*\(') |
              ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
if ($expected.Count -eq 0) { Write-Error "check-exports: no TBLOG_API function found in $Header"; exit 2 }

# Actual: names in the DLL's export table.
$exports = @()
if ((Split-Path -Leaf $Tool) -match '^dumpbin') {
    $lines = & $Tool /nologo /exports $Dll
    if ($LASTEXITCODE -ne 0) { Write-Error "check-exports: $Tool failed"; exit 2 }
    $inTable = $false
    foreach ($l in $lines) {
        if ($l -match '^\s*ordinal\s+hint\s+RVA\s+name') { $inTable = $true; continue }
        if (-not $inTable) { continue }
        if ($l -match '^\s*Summary') { break }
        # "  1    0 00001000 tblog_add_sink" (Debug: "... = @ILT+5(tblog_add_sink)")
        if ($l -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+([A-Za-z_][\w@?$]*)') { $exports += $Matches[1] }
    }
} else {
    $lines = & $Tool -p $Dll
    if ($LASTEXITCODE -ne 0) { Write-Error "check-exports: $Tool failed"; exit 2 }
    $inTable = $false
    foreach ($l in $lines) {
        if ($l -match '^\[Ordinal/Name Pointer\] Table') { $inTable = $true; continue }
        if (-not $inTable) { continue }
        if ($l -match '^\s*$') { break }
        # "\t[   0] +base[   1]  0000 tblog_add_sink" (older binutils: "\t[   0] tblog_add_sink")
        if ($l -match '^\s*\[\s*\d+\].*\s(\S+)\s*$') { $exports += $Matches[1] }
    }
}
$exports = @($exports | Sort-Object -Unique)

$missing = @($expected | Where-Object { $exports -notcontains $_ })
$extra = @($exports | Where-Object { $expected -notcontains $_ })
$status = 0
if ($exports.Count -eq 0) { Write-Host "check-exports: $Dll exports nothing"; $status = 1 }
if ($missing.Count -gt 0) {
    Write-Host "check-exports: declared with TBLOG_API but not exported by ${Dll}:"
    $missing | ForEach-Object { Write-Host "  $_" }
    $status = 1
}
if ($extra.Count -gt 0) {
    Write-Host "check-exports: $Dll exports symbols that are not public API:"
    $extra | ForEach-Object { Write-Host "  $_" }
    $status = 1
}
if ($status -eq 0) {
    Write-Host "check-exports: OK, $($exports.Count) exported: $($exports -join ' ')"
}
exit $status
