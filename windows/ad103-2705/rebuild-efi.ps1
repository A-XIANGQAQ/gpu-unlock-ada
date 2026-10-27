# Rebuild the experimental EFI from any commit, then check its hash.
#
# Why: on 2026-10-28 the USB was overwritten three times and the one build that
# had ever unlocked this card (76FEE036, the build RUN0005 and RUN0006 ran) was
# gone from disk. It turned out to be reproducible: commit cb3691b plus the
# untracked local/boot_profile.h gives that hash byte for byte. So a hardware
# bisect is possible -- build any commit, put it on the stick, boot it.
#
# Usage:
#   .\rebuild-efi.ps1 -Commit cb3691b
#   .\rebuild-efi.ps1 -Commit 6adb3ee -Out G:\tmp\test.efi
#   .\rebuild-efi.ps1 -List
#
# Requires: the repo at -Repo, MSVC x64 tools (Build-Experimental.ps1 finds them
# through vswhere), gcc on PATH or -Gcc.

param(
    [string]$Repo = 'G:\workspace\gpu-unlock-ada',
    [string]$Commit = 'cb3691b',
    [string]$Out,
    [string]$Gcc = 'gcc',
    [switch]$List
)

$ErrorActionPreference = 'Stop'

# Hashes observed on real hardware, so a rebuild can be checked against them.
# The whole local history, rebuilt and hashed on 2026-10-28:
#
#   cb3691b  104448  76FEE036  RUN0005 and RUN0006 unlocked this card
#   c794f3e  104448  76FEE036  same bytes as cb3691b -- the commit touched only
#                              the README, so the binary did not move
#   cef0935  104960  EE440D7F  first build that accepts the blank entry state
#   cdd55be  104960  EE440D7F  same bytes as cef0935
#   6df9735  104960  64D70365  test_boot_io fix
#   6a82336  104448  950689CB  graph-drift workarounds removed
#   6adb3ee  104448  AFB6524A  blank-selector comment corrected
#
# Only 76FEE036 has ever unlocked anything. Everything from cef0935 on has never
# been booted, so a bisect between c794f3e and cef0935 separates "the build that
# works" from "the build that accepts what the 320 W override leaves behind".
$Known = @{
    '76FEE0365338977DA4DE2E29593E1F8EC8FDA0447D0364B7240632CA985F7D29' = 'cb3691b / c794f3e  the only build that ever unlocked this card (RUN0005, RUN0006)'
    'EE440D7F898938283E778ADA36D58C87427E3662918EE28AB521C8E0DC27619E' = 'cef0935 / cdd55be  first blank-selector build, never booted'
    '64D7036565781C843F013A9F9AAC004EDBDCCDD6BD628CEF6D14319B2D0D9F49' = '6df9735  never booted'
    '950689CBDB85E87CCDD13303B4F03C7C95E799B1EA77F318DEDBB2384CA8BAB9' = '6a82336  never booted'
    'AFB6524A381FCA131009A72F085F867E061152E35B1BD70EE0487B47B2B7772D' = '6adb3ee  never booted; the one on the stick as of 2026-10-28 03:22'
}

if ($List) {
    git -C $Repo log --date=format:'%m-%d %H:%M' --pretty=format:'%h %ad %s' cb3691b~1..HEAD
    exit 0
}

$scratch = Join-Path ([System.IO.Path]::GetTempPath()) ('efi-rebuild-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$wt = Join-Path $scratch 'wt'
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
$env:TMP = $scratch
$env:TEMP = $scratch

try {
    # $ErrorActionPreference='Stop' turns any native stderr line into a
    # terminating error, and git writes progress there. Relax it for the call
    # and judge by the exit code instead.
    $eap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        git -C $Repo worktree add --detach --quiet $wt $Commit 2>&1 | Out-Null
        $wc = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $eap
    }
    if ($wc -ne 0) { throw "git worktree add failed for $Commit (exit $wc)" }

    $ad = Join-Path $wt 'windows\ad103-2705'

    # local/boot_profile.h is excluded from git but required by boot_io.c. The
    # main checkout is the only place it lives.
    $local = Join-Path $Repo 'windows\ad103-2705\local'
    if (Test-Path $local) {
        Copy-Item $local (Join-Path $ad 'local') -Recurse -Force
    } else {
        throw "missing $local -- every build needs it, without it boot_io.c fails on local/boot_profile.h"
    }

    Push-Location $ad
    try {
        & powershell -NoProfile -ExecutionPolicy Bypass -File .\Build-Experimental.ps1 -Gcc $Gcc 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Build-Experimental.ps1 failed' }
    } finally {
        Pop-Location
    }

    $efi = Join-Path $ad 'build\AD104-EXPERIMENTAL.EFI'
    if (!(Test-Path $efi)) { throw "no EFI produced at $efi" }

    $item = Get-Item $efi
    $hash = (Get-FileHash $efi -Algorithm SHA256).Hash
    $note = if ($Known.ContainsKey($hash)) { $Known[$hash] } else { 'never seen on this machine' }

    Write-Output ("commit  : {0}" -f (git -C $wt rev-parse --short HEAD))
    Write-Output ("size    : {0} B" -f $item.Length)
    Write-Output ("sha256  : {0}" -f $hash)
    Write-Output ("known   : {0}" -f $note)

    if ($Out) {
        Copy-Item $efi $Out -Force
        Write-Output ("wrote   : {0}" -f $Out)
    }
} finally {
    if (Test-Path $wt) { git -C $Repo worktree remove --force $wt 2>&1 | Out-Null }
    Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue
}
