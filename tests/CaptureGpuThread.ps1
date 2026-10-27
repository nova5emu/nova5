# Native ETW CPU sampling, outside the emulator. Run after reaching the fixed view.
# Requires elevated PowerShell for collection. Never stops a pre-existing session.
[CmdletBinding()]
param(
    [string]$LogPath,
    [string]$OutputBase,
    [ValidateRange(1,60)][int]$Seconds = 30,
    [string]$PdbPath,
    [string]$Scene = 'Astro Bot Sky Garden landing, fixed camera',
    [string]$CacheState = 'warm',
    [switch]$ValidateOnly,
    [switch]$AnalyzeOnly,
    [switch]$NoAnalyze,
    [switch]$SelfTest
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Read-GpuMarker([string]$Path) {
    $marker = $null
    $stream = [IO.File]::Open((Resolve-Path -LiteralPath $Path).Path, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader = [IO.StreamReader]::new($stream)
    try {
        while ($null -ne ($line = $reader.ReadLine())) {
            # Do not reuse a previous run's marker if the newest run has the flag off.
            if ($line.StartsWith('[DIAGNOSTIC_RUN]')) { $marker = $null }
            if ($line.StartsWith('[GPU_THREAD_SAMPLER] ')) {
                $marker = $line.Substring('[GPU_THREAD_SAMPLER] '.Length) | ConvertFrom-Json
            }
        }
    } finally { $reader.Dispose() }
    if ($null -eq $marker -or $marker.schema -ne 1 -or $marker.backend -notin @('etw', 'inprocess')) {
        throw 'No GPU marker in the latest run. Launch with KYTY_GPU_THREAD_SAMPLER=1.'
    }
    return $marker
}

function Get-GpuTarget($Marker) {
    $target = Get-Process -Id ([int]$Marker.pid)
    if ($target.StartTime.ToUniversalTime().ToFileTimeUtc().ToString() -cne $Marker.process_start_filetime) {
        throw 'Stale marker: process creation time differs (PID reused).'
    }
    $lane = @($target.Threads | Where-Object Id -EQ ([int]$Marker.tid))
    if ($lane.Count -ne 1 -or
        $lane[0].StartTime.ToUniversalTime().ToFileTimeUtc().ToString() -cne $Marker.thread_start_filetime) {
        throw 'Stale marker: GPU thread is absent or its creation time differs (TID reused).'
    }
    return $target
}

function File-Identity([string]$Path) {
    $file = Get-Item -LiteralPath $Path
    return [ordered]@{ path=$file.FullName; bytes=$file.Length; sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash }
}

function Invoke-Xperf([string[]]$Arguments) {
    ('xperf ' + ($Arguments -join ' ')) | Out-File -LiteralPath $script:CommandLog -Append -Encoding utf8
    # Windows PowerShell wraps native stderr as error records, even on success.
    # Do not mistake a warning after successful StartTrace for a failed start.
    $savedPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $messages = & $script:Xperf @Arguments 2>&1
        $nativeExit = $LASTEXITCODE
    } finally { $ErrorActionPreference = $savedPreference }
    $messages | Out-File -LiteralPath $script:CommandLog -Append -Encoding utf8 -ErrorAction Continue
    if ($nativeExit -ne 0) { throw "xperf failed ($nativeExit). See $script:CommandLog" }
}

if ($SelfTest) {
    # Exercise marker selection and PID/TID reuse rejection without starting ETW.
    $fixture = [IO.Path]::GetTempFileName()
    try {
        $current = Get-Process -Id $PID
        $lane = @($current.Threads)[0]
        $marker = [pscustomobject]@{ schema=1; backend='etw'; pid=$PID; tid=$lane.Id
            process_start_filetime=$current.StartTime.ToUniversalTime().ToFileTimeUtc().ToString()
            thread_start_filetime=$lane.StartTime.ToUniversalTime().ToFileTimeUtc().ToString() }
        $line = '[GPU_THREAD_SAMPLER] ' + ($marker | ConvertTo-Json -Compress)
        [IO.File]::WriteAllText($fixture, "[DIAGNOSTIC_RUN]`n$line`n")
        # The real helper keeps stdout open for writing while we collect.
        $writer = [IO.File]::Open($fixture, [IO.FileMode]::Append, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)
        try { $read = Read-GpuMarker $fixture } finally { $writer.Dispose() }
        if ((Get-GpuTarget $read).Id -ne $PID) { throw 'Live marker rejected' }
        $marker.backend = 'inprocess'
        $line = '[GPU_THREAD_SAMPLER] ' + ($marker | ConvertTo-Json -Compress)
        [IO.File]::WriteAllText($fixture, "[DIAGNOSTIC_RUN]`n$line`n")
        $read = Read-GpuMarker $fixture
        if ($read.backend -ne 'inprocess' -or (Get-GpuTarget $read).Id -ne $PID) { throw 'In-process marker rejected' }
        $read.process_start_filetime = '1'
        $rejected = $false
        try { $null = Get-GpuTarget $read } catch { $rejected = $true }
        if (-not $rejected) { throw 'Reused PID accepted' }
        $read = Read-GpuMarker $fixture
        $read.thread_start_filetime = '1'
        $rejected = $false
        try { $null = Get-GpuTarget $read } catch { $rejected = $true }
        if (-not $rejected) { throw 'Reused TID accepted' }
        [IO.File]::AppendAllText($fixture, "[DIAGNOSTIC_RUN]`n")
        $rejected = $false
        try { $null = Read-GpuMarker $fixture } catch { $rejected = $true }
        if (-not $rejected) { throw 'Previous-run marker accepted' }
        Write-Output 'CaptureGpuThread: marker selection, PID and TID lifetime tests passed.'
    } finally { Remove-Item -LiteralPath $fixture }
    return
}

if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit PowerShell.' }
if (-not $OutputBase) { throw 'Provide -OutputBase in an existing evidence directory.' }
$OutputBase = [IO.Path]::GetFullPath($OutputBase)
$outputDir = Split-Path -Parent $OutputBase
if (-not (Test-Path -LiteralPath $outputDir -PathType Container)) { throw 'Output directory must already exist.' }
$script:Xperf = (Get-Command xperf.exe -ErrorAction Stop).Source
$script:CommandLog = "$OutputBase.commands.txt"
$trace = "$OutputBase.etl"
$manifestPath = "$OutputBase.json"

if (-not $AnalyzeOnly) {
    if (-not $LogPath) { throw 'Provide -LogPath containing the current GPU marker.' }
    $marker = Read-GpuMarker $LogPath
    $target = Get-GpuTarget $marker
    $executable = $target.MainModule.FileName
    if ((Get-Item -LiteralPath $executable).LastWriteTimeUtc -gt $target.StartTime.ToUniversalTime()) {
        throw 'Executable changed after process launch. Restart with the intended build.'
    }
    if (-not $PdbPath) { $PdbPath = Join-Path (Split-Path -Parent $executable) 'kyty_emulator.pdb' }
    $manifest = [ordered]@{ schema=1; backend='etw'; target=$marker; scene=$Scene; cache=$CacheState
        requested_seconds=$Seconds; executable=(File-Identity $executable); pdb=(File-Identity $PdbPath)
        flags='PROC_THREAD+LOADER+PROFILE+CSWITCH'; stackwalk='Profile'; profile_interval='existing system interval (not changed)'
        start_utc=$null; end_utc=$null; elapsed_seconds=0; status='validated'; target_exited=$false
        target_thread_changed=$false }
    $config = Join-Path (Split-Path -Parent $executable) 'Nova5.ini'
    if (Test-Path -LiteralPath $config) { $manifest.config = File-Identity $config }
    Write-Output "GPU target PID=$($marker.pid), TID=$($marker.tid); exe=$($manifest.executable.sha256)"
    if ($ValidateOnly) {
        Write-Output 'Validated identity, xperf and local PDB presence. No trace started; PDB matching is checked by ETW symbol decoding.'
        return
    }
    $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'ETW kernel collection requires elevated PowerShell. This script does not elevate itself.'
    }
    foreach ($suffix in @('.etl','.raw.etl','.json','.commands.txt')) {
        if (Test-Path -LiteralPath "$OutputBase$suffix") { throw "Evidence already exists: $OutputBase$suffix" }
    }
    # Hashing/preflight are outside the recording window. Revalidate before starting.
    $null = Get-GpuTarget $marker
    $owned = $false
    try {
        # Start is atomic: an occupied kernel logger fails; do NOT stop it first.
        Invoke-Xperf -Arguments @('-start','NT Kernel Logger','-on',$manifest.flags,'-stackwalk','Profile',
            '-BufferSize','128','-MinBuffers','64','-MaxBuffers','256','-MaxFile','512',
            '-FileMode','Sequential','-f',"$OutputBase.raw.etl")
        $owned = $true
        $manifest.status = 'recording'
        $manifest.start_utc = [DateTime]::UtcNow.ToString('o')
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding utf8
        Write-Output "Recording $Seconds seconds. Keep the camera fixed."
        while ($watch.Elapsed.TotalSeconds -lt $Seconds) {
            Start-Sleep -Milliseconds 200
            $target.Refresh()
            if ($target.HasExited) { $manifest.target_exited = $true; break }
        }
        $manifest.elapsed_seconds = $watch.Elapsed.TotalSeconds
        try { $null = Get-GpuTarget $marker } catch { $manifest.target_thread_changed = $true }
        $manifest.status = 'captured'
    } finally {
        if ($owned) {
            # This finally also handles Ctrl+C. Stop ONLY the session we started.
            try { Invoke-Xperf -Arguments @('-stop','NT Kernel Logger') }
            catch { $manifest.status = 'stop_failed'; throw }
            finally {
                $manifest.end_utc = [DateTime]::UtcNow.ToString('o')
                if ($manifest.status -eq 'recording') { $manifest.status = 'interrupted' }
                $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding utf8
            }
        }
    }
    # Required for trace image/CodeView identity and reliable symbol matching.
    Invoke-Xperf -Arguments @('-merge',"$OutputBase.raw.etl",$trace,'-compress')
    $manifest.status = 'merged'
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding utf8
    if ($manifest.target_exited -or $manifest.target_thread_changed) {
        throw 'Target ended or changed during capture. Raw trace saved; do not combine samples across thread lifetimes.'
    }
    if ($NoAnalyze) { Write-Output "Saved $trace and $manifestPath"; return }
} else {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $marker = $manifest.target
}

# Decode after collection, with local symbols only. Never force a mismatched PDB.
foreach ($identity in @($manifest.executable, $manifest.pdb)) {
    if ((Get-FileHash -LiteralPath $identity.path -Algorithm SHA256).Hash -ne $identity.sha256) {
        throw "Binary/PDB changed since capture: $($identity.path). Keep raw ETL; do not attribute it to this build."
    }
}
$savedSymbol = $env:_NT_SYMBOL_PATH
$savedAlternate = $env:_NT_ALT_SYMBOL_PATH
$savedCache = $env:_NT_SYMCACHE_PATH
try {
    $env:_NT_SYMBOL_PATH = Split-Path -Parent $manifest.pdb.path
    $env:_NT_ALT_SYMBOL_PATH = ''
    $env:_NT_SYMCACHE_PATH = $outputDir
    Invoke-Xperf -Arguments @('-i',$trace,'-o',"$OutputBase.stats.txt",'-a','tracestats','-timespan','-detail','stack')
    Invoke-Xperf -Arguments @('-i',$trace,'-o',"$OutputBase.frequency.txt",'-a','profile','-freq')
    Invoke-Xperf -Arguments @('-i',$trace,'-o',"$OutputBase.threads.csv",'-a','cswitch','-thread')
    Invoke-Xperf -Arguments @('-i',$trace,'-o',"$OutputBase.symbols.txt",'-a','symcache','-dbgid')
    Invoke-Xperf -Arguments @('-i',$trace,'-symbols','-o',"$OutputBase.gpu-stacks.txt",'-a','stack',
        '-butterfly','1','-pid',([string]$marker.pid),'-tid',([string]$marker.tid))
    Write-Output "Saved GPU-only stacks: $OutputBase.gpu-stacks.txt"
    Write-Output 'Check trace loss, sampling interval, unknown/JIT samples and thread CPU time before interpreting percentages.'
    Write-Output "WPA: open $trace; CPU Usage (Sampled); filter PID=$($marker.pid), TID=$($marker.tid)."
} finally {
    $env:_NT_SYMBOL_PATH = $savedSymbol
    $env:_NT_ALT_SYMBOL_PATH = $savedAlternate
    $env:_NT_SYMCACHE_PATH = $savedCache
}
