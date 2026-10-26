# Builds _Build/package/Nova5-Setup.exe from the compiled emulator in _Build/nova5 and the Nova5 launcher.
# Run after compiling _Build/nova5 (Nova5Emulator and atrac9). The setup is replaced on every run.
param(
	[string]$Build = (Join-Path $PSScriptRoot '..\..\..\_Build\nova5')
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
foreach ($file in 'Nova5Emulator.exe', 'libwinpthread-1.dll', 'atrac9.dll') {
	Copy-Item (Join-Path $build $file) $stage
}
Copy-Item (Join-Path $repo 'assets\cursors') (Join-Path $stage 'assets\cursors') -Recurse
New-Item -ItemType Directory -Force (Join-Path $stage 'assets\sounds') | Out-Null
Copy-Item (Join-Path $build 'assets\sounds\trophy-unlock.wav') (Join-Path $stage 'assets\sounds')

# Visual C++ runtime, app-local (redistributable), so no separate installer or admin rights are needed.
$redist = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT' -Directory |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $redist) { throw 'Visual C++ redistributable files not found' }
Copy-Item (Join-Path $redist.FullName '*.dll') $stage

# Licenses: GPL-2.0 for the emulator, Kyty's MIT notice and every third-party license.
Copy-Item (Join-Path $repo 'LICENSE') (Join-Path $stage 'LICENSE.txt')
$licenses = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Force $licenses | Out-Null
Copy-Item (Join-Path $repo 'LICENSES\*') $licenses
# Notices of the prebuilt FFmpeg, Opus and OpenXR libraries the emulator links (LGPL/BSD/Apache).
Copy-Item (Join-Path $build 'licenses\*') $licenses -Recurse
foreach ($dir in Get-ChildItem (Join-Path $repo '3rdparty') -Directory) {
	Get-ChildItem $dir.FullName -File | Where-Object { $_.Name -match '^(LICEN[CS]E|COPYING|NOTICE)' } | ForEach-Object {
		Copy-Item $_.FullName (Join-Path $licenses "$($dir.Name)-$($_.BaseName).txt")
	}
}
Copy-Item (Join-Path $PSScriptRoot 'LEEME.txt') $stage

# PKG installer: pkg-to-anyps5 (GPL-3.0, https://github.com/awake-devel/pkg-to-anyps5), run by the launcher.
$extractor = Join-Path $repo '..\herramientas\pkg-extractor\pkg-to-anyps5-v0.2.5-windows-x86_64'
New-Item -ItemType Directory -Force (Join-Path $stage 'tools') | Out-Null
Copy-Item (Join-Path $extractor 'pkg-to-anyps5.exe') (Join-Path $stage 'tools')
Copy-Item (Join-Path $extractor 'LICENSE') (Join-Path $licenses 'pkg-to-anyps5-LICENSE.txt')
Copy-Item (Join-Path $extractor 'README.md') (Join-Path $licenses 'pkg-to-anyps5-README.md')

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
