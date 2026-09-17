#Requires -Version 5.1
<#
.SYNOPSIS
  Fetches the third-party headers this project builds against.
.DESCRIPTION
  Nothing fetched here is redistributed in the repository. Each dependency is pinned to an exact
  tag so a build is reproducible. ReShade resolves the Dear ImGui function table by exact
  IMGUI_VERSION_NUM, so the ReShade and ImGui versions must be changed together -- see
  docs/compatibility.md.
#>
[CmdletBinding()]
param(
  [string]$ReShadeTag = 'v6.4.1',   # RESHADE_API_VERSION 16
  [string]$ImGuiTag   = 'v1.91.8-docking', # IMGUI_VERSION_NUM 19180, docking branch
  [string]$Ts3Ref     = 'master',
  [string]$Ts3Commit  = ''
)

$ErrorActionPreference = 'Stop'
$root   = Split-Path -Parent $PSScriptRoot
$vendor = Join-Path $root 'third_party'
New-Item -ItemType Directory -Force -Path $vendor | Out-Null

function Fetch-Sparse([string]$Url, [string]$Dir, [string]$Ref, [string]$Path) {
  if (Test-Path (Join-Path $Dir '.git')) { Write-Host "==> $Dir already present; skipping"; return }
  Write-Host "==> fetching $Url @ $Ref"
  if (Test-Path $Dir) { Remove-Item -Recurse -Force $Dir }
  git clone --depth 1 --filter=blob:none --sparse --branch $Ref $Url $Dir
  if ($LASTEXITCODE -ne 0) { throw "git clone failed for $Url" }
  git -C $Dir sparse-checkout set $Path
}

Fetch-Sparse 'https://github.com/crosire/reshade.git' (Join-Path $vendor 'reshade') $ReShadeTag 'include'

$imgui = Join-Path $vendor 'imgui'
if (Test-Path (Join-Path $imgui '.git')) {
  Write-Host "==> $imgui already present; skipping"
} else {
  Write-Host "==> fetching Dear ImGui @ $ImGuiTag"
  if (Test-Path $imgui) { Remove-Item -Recurse -Force $imgui }
  git clone --depth 1 --branch $ImGuiTag 'https://github.com/ocornut/imgui.git' $imgui
  if ($LASTEXITCODE -ne 0) { throw 'git clone failed for Dear ImGui' }
}

$sdk = Join-Path $vendor 'ts3client-pluginsdk'
if (Test-Path (Join-Path $sdk '.git')) {
  Write-Host "==> $sdk already present; skipping"
} else {
  Write-Host '==> fetching the TeamSpeak 3 Plugin SDK'
  if (Test-Path $sdk) { Remove-Item -Recurse -Force $sdk }
  git clone --depth 1 --branch $Ts3Ref 'https://github.com/TeamSpeak-Systems/ts3client-pluginsdk.git' $sdk
  if ($LASTEXITCODE -ne 0) { throw 'git clone failed for the TeamSpeak Plugin SDK' }
  if ($Ts3Commit) {
    git -C $sdk fetch --depth 1 origin $Ts3Commit
    git -C $sdk checkout $Ts3Commit
  }
}

Write-Host ''
Write-Host "Dependencies are in ${vendor}:"
Write-Host "  ReShade SDK   $ReShadeTag"
Write-Host "  Dear ImGui    $ImGuiTag"
Write-Host "  TeamSpeak SDK $(if ($Ts3Commit) { $Ts3Commit } else { $Ts3Ref })"
Write-Host ''
Write-Host 'Verify the ImGui pairing before building the add-on -- these two numbers must be equal:'
Select-String -Path (Join-Path $vendor 'reshade/include/reshade_overlay.hpp') -Pattern 'IMGUI_VERSION_NUM !=' | Select-Object -First 1
Select-String -Path (Join-Path $vendor 'imgui/imgui.h') -Pattern 'define IMGUI_VERSION_NUM' | Select-Object -First 1
