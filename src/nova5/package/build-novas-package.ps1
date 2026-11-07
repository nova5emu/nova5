# Builds _Build/package-novas/NovaS-Setup.exe and NovaS-Portable.zip: the Switch 2 emulator on its own.
# NovaS.exe is the Nova5 launcher published under its NovaS name (Edition.cs picks the NovaS-only mode from it) with its
# own version and icon; novas\NovaSEmulator.exe is the engine built by novas\compilar-publico.cmd (no PC paths inside).
param(
	[string]$Version = '0.1.0',
	[string]$Engine = $(Join-Path $PSScriptRoot '..\..\..\..\novas\codigo\_Build\publico\bin\NovaSEmulator.exe'),
	[string]$EngineSource = $(Join-Path $PSScriptRoot '..\..\..\..\novas\codigo')
)
$ErrorActionPreference = 'Stop'
$repo    = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$engine  = (Resolve-Path $Engine).Path
$source  = (Resolve-Path $EngineSource).Path
$package = Join-Path $repo '_Build\package-novas'
$stage   = Join-Path $package 'stage'
$publish = Join-Path $package 'publish'

foreach ($dir in $stage, $publish) { if (Test-Path $dir) { Remove-Item $dir -Recurse -Force } }
New-Item -ItemType Directory -Force $stage, $publish | Out-Null

# Launcher: the same single-file executable as Nova5, with NovaS's version and icon, renamed to NovaS.exe.
$project = Join-Path $PSScriptRoot '..\Nova5.csproj'
dotnet publish $project -c Release -o $publish --nologo -v quiet "-p:Version=$Version" "-p:ApplicationIcon=Assets\novas.ico" `
	"-p:Product=NovaS" "-p:DebugType=none" "-p:PathMap=$repo=/_/" "-p:IntermediateOutputPath=$package\obj\" "-p:BaseOutputPath=$package\bin\"
if ($LASTEXITCODE -ne 0) { throw "dotnet publish failed ($LASTEXITCODE)" }
Copy-Item (Join-Path $publish 'Nova5.exe') (Join-Path $stage 'NovaS.exe')

# Engine, with the Visual C++ runtime next to it (app-local, no separate installer or admin rights needed).
$novas = Join-Path $stage 'novas'
New-Item -ItemType Directory -Force $novas | Out-Null
Copy-Item $engine $novas
$redist = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT' -Directory |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $redist) { throw 'Visual C++ redistributable files not found' }
Copy-Item (Join-Path $redist.FullName '*.dll') $novas

# Licenses: GPL-3.0 for the engine (Eglantine, from Eden, yuzu and Sudachi) and its third-party notices, and the
# launcher's GPL-2.0-or-later.
Copy-Item (Join-Path $source 'LICENSE.txt') (Join-Path $stage 'LICENSE.txt')
$licenses = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Force $licenses | Out-Null
Copy-Item (Join-Path $source 'LICENSES\*') $licenses
Copy-Item (Join-Path $repo 'LICENSE') (Join-Path $licenses 'NovaS-launcher-GPL-2.0.txt')
Copy-Item (Join-Path $PSScriptRoot 'LEEME-NovaS.txt') (Join-Path $stage 'LEEME.txt')

# Nothing of this PC may travel with it.
foreach ($file in Get-ChildItem $stage -Recurse -File) {
	$text = [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes($file.FullName))
	if ($text -match 'Users[\\/]Oscar' -or $text -match 'oscargaldamez') { throw "Private path inside $($file.FullName)" }
}

$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe') |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 not found (winget install JRSoftware.InnoSetup)' }
& $iscc /Q "/DStage=$stage" "/DOutput=$package" "/DAppVersion=$Version" (Join-Path $PSScriptRoot 'NovaS.iss')
if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }

$portable = Join-Path $package 'NovaS-Portable.zip'
if (Test-Path $portable) { Remove-Item $portable }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $portable -CompressionLevel Optimal
Get-Item (Join-Path $package 'NovaS-Setup.exe'), $portable
