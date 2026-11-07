# Builds _Build/package/Nova5-Setup.exe from the compiled emulator in _Build/nova5 and the Nova5 launcher.
# Run after compiling _Build/nova5 (Nova5Emulator and atrac9). The setup is replaced on every run.
# -Build: the compiled emulator (_Build\nova5, else the unified engine's _Build\windows). -Tools: the folder that holds
# pkg-extractor (the repository's ..\herramientas by default).
param(
	[string]$Build = $(foreach ($dir in '_Build\nova5', '_Build\windows') {
		$path = Join-Path $PSScriptRoot "..\..\..\$dir"
		if (Test-Path (Join-Path $path 'Nova5Emulator.exe')) { $path; break }
	}),
	[string]$Tools = ''
)
$ErrorActionPreference = 'Stop'
$repo    = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$build   = (Resolve-Path $Build).Path
$package = Join-Path $repo '_Build\package'
$stage   = Join-Path $package 'stage'

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

# Launcher: one self-contained executable.
$project = Join-Path $PSScriptRoot '..\Nova5.csproj'
dotnet publish $project -c Release -o $stage --nologo -v quiet
if ($LASTEXITCODE -ne 0) { throw "dotnet publish failed ($LASTEXITCODE)" }
[xml]$csproj = Get-Content $project
$version = $csproj.Project.PropertyGroup.Version | Where-Object { $_ } | Select-Object -First 1

# Emulator runtime, as installed by the CMake install rules.
Copy-Item (Join-Path $build 'Nova5Emulator.exe') $stage
# atrac9.dll: the ATRAC9 decoder the launcher's menu music loads (the atrac9 CMake target).
foreach ($file in 'libwinpthread-1.dll', 'atrac9.dll') {
	if (Test-Path (Join-Path $build $file)) { Copy-Item (Join-Path $build $file) $stage }
}
Copy-Item (Join-Path $repo 'assets\cursors') (Join-Path $stage 'assets\cursors') -Recurse
New-Item -ItemType Directory -Force (Join-Path $stage 'assets\sounds') | Out-Null
$sound = @((Join-Path $build 'assets\sounds\trophy-unlock.wav'), (Join-Path $repo 'assets\sounds\trophy-unlock.wav')) |
    Where-Object { Test-Path $_ } | Select-Object -First 1
Copy-Item $sound (Join-Path $stage 'assets\sounds')

# Visual C++ runtime, app-local (redistributable), so no separate installer or admin rights are needed.
$redist = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT' -Directory |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $redist) { throw 'Visual C++ redistributable files not found' }
Copy-Item (Join-Path $redist.FullName '*.dll') $stage

# NovaS (the Switch 2 tab): its engine from novas\compilar-publico.cmd (no PC paths inside), with the same runtime next
# to it, and its GPL-3.0 licenses.
$novasSource = Join-Path $repo '..\novas\codigo'
$novasEngine = Join-Path $novasSource '_Build\publico\bin\NovaSEmulator.exe'
if (-not (Test-Path $novasEngine)) { throw 'NovaS engine not built (run novas\compilar-publico.cmd)' }
$novasDir = Join-Path $stage 'novas'
New-Item -ItemType Directory -Force $novasDir | Out-Null
Copy-Item $novasEngine $novasDir
Copy-Item (Join-Path $redist.FullName '*.dll') $novasDir

# Licenses: GPL-2.0 for the emulator, Kyty's MIT notice and every third-party license.
Copy-Item (Join-Path $repo 'LICENSE') (Join-Path $stage 'LICENSE.txt')
$licenses = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Force $licenses | Out-Null
Copy-Item (Join-Path $repo 'LICENSES\*') $licenses
# Notices of the prebuilt FFmpeg, Opus and OpenXR libraries the emulator links (LGPL/BSD/Apache).
if (Test-Path (Join-Path $build 'licenses')) { Copy-Item (Join-Path $build 'licenses\*') $licenses -Recurse }
foreach ($dir in Get-ChildItem (Join-Path $repo '3rdparty') -Directory) {
	Get-ChildItem $dir.FullName -File | Where-Object { $_.Name -match '^(LICEN[CS]E|COPYING|NOTICE)' } | ForEach-Object {
		Copy-Item $_.FullName (Join-Path $licenses "$($dir.Name)-$($_.BaseName).txt")
	}
}
Copy-Item (Join-Path $PSScriptRoot 'LEEME.txt') $stage
New-Item -ItemType Directory -Force (Join-Path $licenses 'novas') | Out-Null
Copy-Item (Join-Path $novasSource 'LICENSE.txt') (Join-Path $licenses 'novas\LICENSE.txt')
Copy-Item (Join-Path $novasSource 'LICENSES\*') (Join-Path $licenses 'novas')

# PKG installer: pkg-to-anyps5 (GPL-3.0, https://github.com/awake-devel/pkg-to-anyps5), run by the launcher.
# v0.2.5 source with pkg-to-anyps5.patch applied, built with cargo build --release.
$extractor = Join-Path $(if ($Tools) { $Tools } else { Join-Path $repo '..\herramientas' }) 'pkg-extractor\pkg-to-anyps5-src'
New-Item -ItemType Directory -Force (Join-Path $stage 'tools') | Out-Null
Copy-Item (Join-Path $extractor 'target\release\pkg-to-anyps5.exe') (Join-Path $stage 'tools')
# .zar packer (Exzap/ZArchive, MIT-0; the zarchive_pack CMake target), run from a game's options.
if (Test-Path (Join-Path $build 'tools\zarchive.exe')) { Copy-Item (Join-Path $build 'tools\zarchive.exe') (Join-Path $stage 'tools') }
# Shader preparation (Preparar shaders in a game's options): the kyty_shader_precompile program next to the emulator's
# DLLs, its script, and the render-pass states and specialization hints it reads.
if (Test-Path (Join-Path $build 'kyty_shader_precompile.exe')) {
	Copy-Item (Join-Path $build 'kyty_shader_precompile.exe'), (Join-Path $repo 'precompile-windows.ps1') $stage
	$prepData = Join-Path $stage 'tools\local\static-precompile'
	New-Item -ItemType Directory -Force $prepData | Out-Null
	Copy-Item (Join-Path $repo 'tools\local\static-precompile\pass-states.json'), (Join-Path $repo 'tools\local\static-precompile\hints-*.hints') $prepData
}
Copy-Item (Join-Path $extractor 'LICENSE') (Join-Path $licenses 'pkg-to-anyps5-LICENSE.txt')
Copy-Item (Join-Path $extractor 'README.md') (Join-Path $licenses 'pkg-to-anyps5-README.md')
Copy-Item (Join-Path $PSScriptRoot 'pkg-to-anyps5.patch') (Join-Path $licenses 'pkg-to-anyps5-changes.patch')

$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe') |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 not found (winget install JRSoftware.InnoSetup)' }
& $iscc /Q "/DStage=$stage" "/DOutput=$package" "/DAppVersion=$version" (Join-Path $PSScriptRoot 'Nova5.iss')
if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }

# Portable copy of the same files, for launchers that unpack emulators themselves (Nexo).
$portable = Join-Path $package 'Nova5-Portable.zip'
if (Test-Path $portable) { Remove-Item $portable }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $portable -CompressionLevel Optimal
Get-Item (Join-Path $package 'Nova5-Setup.exe'), $portable
