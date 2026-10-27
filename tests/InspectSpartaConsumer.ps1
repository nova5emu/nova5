# Read-only live observations for PPSA28997 v01.008.001; never suspends threads.
param(
    [Parameter(Mandatory=$true)][int]$TargetPid,
    [uint64]$ObjectAddress = 0,
    [ValidateRange(1,30)][int]$Samples = 3,
    [switch]$PlaybackState,
    [switch]$CompletionDetails,
    [switch]$StartupState,
    [switch]$FmodDetails,
    [string]$LogPath = (Join-Path $PSScriptRoot '..\..\pruebas\ppsa28997-console.log'),
    [uint64]$ReadAddress = 0,
    [ValidateRange(1,32768)][int]$ReadLength = 256
)
$ErrorActionPreference = 'Stop'
if ($FmodDetails) { $StartupState = $true; $PlaybackState = $true }
if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit PowerShell.' }
if (-not ('SpartaReadOnly' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SpartaReadOnly {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool ReadProcessMemory(IntPtr process, IntPtr address,
        byte[] buffer, UIntPtr size, out UIntPtr read);
    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr handle);
}
'@
}
$process = Get-Process -Id $TargetPid
if ($process.ProcessName -ne 'Nova5Emulator') { throw 'Unexpected process name.' }
$started = $null
if ($null -ne $process.StartTime) {
    $started = $process.StartTime.ToUniversalTime().ToString('o')
}
if ($ObjectAddress -eq 0) {
    $eventLine = Get-Content -LiteralPath $LogPath -Tail 1500 |
        Where-Object { $_ -match 'op=Av.event_enter .*object=0x[0-9a-f]+ .*c=0x90155ba20 ' } |
        Select-Object -Last 1
    if (-not $eventLine -or $eventLine -notmatch 'object=0x([0-9a-f]+)') {
        throw 'No matching callback in recent log; wait for a stall dump.'
    }
    $ObjectAddress = [Convert]::ToUInt64($Matches[1],16)
}
# PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION only.
$handle = [SpartaReadOnly]::OpenProcess(0x1010, $false, $TargetPid)
if ($handle -eq [IntPtr]::Zero) {
    throw "OpenProcess failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
}
function Read-Remote([uint64]$Address, [int]$Length) {
    $buffer = [byte[]]::new($Length)
    $received = [UIntPtr]::Zero
    if (-not [SpartaReadOnly]::ReadProcessMemory($handle,[IntPtr]::new([int64]$Address),
            $buffer,[UIntPtr]::new([uint64]$Length),[ref]$received) -or
            $received.ToUInt64() -ne $Length) {
        throw ('ReadProcessMemory at 0x{0:x} failed: {1}' -f $Address,
            [Runtime.InteropServices.Marshal]::GetLastWin32Error())
    }
    return ,$buffer
}
function Hex([uint64]$Value) { return ('0x{0:x}' -f $Value) }
function Read-AsciiName([uint64]$Address) {
    if ($Address -eq 0) { return $null }
    $b = Read-Remote $Address 128
    $end = [Array]::IndexOf($b,[byte]0)
    if ($end -lt 0) { $end = $b.Length }
    return [Text.Encoding]::UTF8.GetString($b,0,$end)
}
function Read-ManagedString([uint64]$Address) {
    if ($Address -eq 0) { return $null }
    $head = Read-Remote $Address 20
    $length = [BitConverter]::ToInt32($head,16)
    if ($length -lt 0 -or $length -gt 1024) { throw 'Managed string exceeds bounds.' }
    if ($length -eq 0) { return '' }
    return [Text.Encoding]::Unicode.GetString((Read-Remote ($Address+20) ($length*2)))
}
function Read-ManagedStringList([uint64]$Address) {
    if ($Address -eq 0) { return $null }
    $head = Read-Remote $Address 32
    $array = [BitConverter]::ToUInt64($head,16)
    $size = [BitConverter]::ToInt32($head,24)
    $version = [BitConverter]::ToInt32($head,28)
    if ($size -lt 0 -or $size -gt 256) { throw 'String list exceeds bounds.' }
    $items = @()
    if ($size -gt 0) {
        if ($array -eq 0) { throw 'Null string list storage.' }
        $capacity = [BitConverter]::ToUInt64((Read-Remote ($array+24) 8),0)
        if ($capacity -lt $size) { throw 'String list size exceeds capacity.' }
        $data = Read-Remote ($array+32) ($size*8)
        for ($i=0; $i -lt $size; $i++) {
            $ptr = [BitConverter]::ToUInt64($data,$i*8)
            $item = [ordered]@{address=(Hex $ptr)}
            try { $item.text=Read-ManagedString $ptr } catch { $item.error=$_.Exception.Message }
            $items += $item
        }
    }
    $after = Read-Remote $Address 32
    return [ordered]@{address=(Hex $Address); size=$size; items=$items;
        header_stable=([BitConverter]::ToUInt64($after,16) -eq $array -and
            [BitConverter]::ToInt32($after,24) -eq $size -and [BitConverter]::ToInt32($after,28) -eq $version)}
}
function Read-FmodDetails([byte[]]$InstanceData) {
    $result = [ordered]@{note='Registered banks are not a list of pending loads. Handles are opaque; no FMOD methods invoked.'}
    try {
        # AnySampleDataLoading, RVA 0x1016e92..0x1016f33: dictionary entries
        # +0x18, count +0x20, version +0x2c, 32-byte entries, hash<0 is unused.
        $dict = [BitConverter]::ToUInt64($InstanceData,0x60)
        $d = Read-Remote $dict 0x30
        $array = [BitConverter]::ToUInt64($d,0x18)
        $count = [BitConverter]::ToInt32($d,0x20)
        $version = [BitConverter]::ToInt32($d,0x2c)
        if ($count -lt 0 -or $count -gt 256) { throw 'Bank dictionary exceeds bounds.' }
        $entries = @()
        if ($count -gt 0) {
            if ($array -eq 0) { throw 'Null bank dictionary storage.' }
            $capacity = [BitConverter]::ToUInt64((Read-Remote ($array+24) 8),0)
            if ($capacity -lt $count) { throw 'Bank dictionary count exceeds capacity.' }
            $data = Read-Remote ($array+32) ($count*32)
            for ($i=0; $i -lt $count; $i++) {
                if ([BitConverter]::ToInt32($data,$i*32) -lt 0) { continue }
                $key = [BitConverter]::ToUInt64($data,$i*32+8)
                $entry = [ordered]@{index=$i; key=(Hex $key);
                    bank_handle=(Hex ([BitConverter]::ToUInt64($data,$i*32+16)));
                    ref_count=[BitConverter]::ToInt32($data,$i*32+24)}
                try { $entry.name=Read-ManagedString $key } catch { $entry.error=$_.Exception.Message }
                $entries += $entry
            }
        }
        $after = Read-Remote $dict 0x30
        $result.registered_banks=[ordered]@{count_slots=$count; entries=$entries;
            header_stable=([BitConverter]::ToUInt64($after,0x18) -eq $array -and
                [BitConverter]::ToInt32($after,0x20) -eq $count -and [BitConverter]::ToInt32($after,0x2c) -eq $version)}
    } catch { $result.banks_error=$_.Exception.Message }
    try {
        # ExecuteSampleLoadRequestsIfReady, RVA 0x10159xx..0x1015bf5.
        $result.sample_load_requests=Read-ManagedStringList ([BitConverter]::ToUInt64($InstanceData,0x68))
    } catch { $result.sample_requests_error=$_.Exception.Message }
    try {
        # Settings.Instance global verified in RuntimeManager.LoadBank.
        $klass = [BitConverter]::ToUInt64((Read-Remote 0x935e28fe0 8),0)
        $statics = [BitConverter]::ToUInt64((Read-Remote ($klass+0xb8) 8),0)
        $instance = [BitConverter]::ToUInt64((Read-Remote $statics 8),0)
        if ($instance -ne 0) {
            $s = Read-Remote $instance 0xc8
            if ([BitConverter]::ToUInt64($s,0) -ne $klass) { throw 'Settings class differs.' }
            $result.settings=[ordered]@{instance=(Hex $instance);
                import_type=[BitConverter]::ToInt32($s,0x50);
                target_asset_path=(Read-ManagedString ([BitConverter]::ToUInt64($s,0x58)));
                target_bank_folder=(Read-ManagedString ([BitConverter]::ToUInt64($s,0x60)));
                raw_hex=([BitConverter]::ToString($s).Replace('-',''))}
            # BanksToLoad.MoveNext (0x101b020): settings+0x40 selects the
            # +0xb0/+0xb8 or +0xc0 lists. Names also verified in metadata.
            $result.settings.bank_load_type=[BitConverter]::ToInt32($s,0x40)
            foreach ($field in @(@('master_banks',0xb0),@('banks',0xb8),@('banks_to_load',0xc0))) {
                try { $result.settings[$field[0]]=Read-ManagedStringList ([BitConverter]::ToUInt64($s,$field[1])) }
                catch { $result.settings[$field[0]]=[ordered]@{error=$_.Exception.Message} }
            }
        }
    } catch { $result.settings_error=$_.Exception.Message }
    $result.bindings=[ordered]@{}
    foreach ($binding in @(@('loadBankFile',0x93627e600L),@('loadSampleData',0x93627e9e0L),@('getSampleLoadingState',0x93627e9f8L))) {
        try { $result.bindings[$binding[0]]=Hex ([BitConverter]::ToUInt64((Read-Remote $binding[1] 8),0)) }
        catch { $result.bindings[$binding[0]]=$_.Exception.Message }
    }
    return $result
}
function Read-StartupState([byte[]]$ComponentData) {
    # These offsets are derived from this game's eboot/Il2cppUserAssemblies,
    # not a general Unity object walker. Never invoke managed/native methods.
    $result = [ordered]@{note='Non-atomic, bounded owner/ancestor and FMOD snapshots; no guest calls.'}
    $owner = [BitConverter]::ToUInt64($ComponentData,0x30)
    $videoObject = [BitConverter]::ToUInt64($ComponentData,0x28)
    $range = Read-Remote 0x901fd4768 8
    $transformMin = [BitConverter]::ToUInt32($range,0)
    $transformCount = [BitConverter]::ToUInt32($range,4)
    $owners = @(); $seen = @{}
    for ($depth=0; $depth -lt 8 -and $owner -ne 0; $depth++) {
        if ($seen.ContainsKey($owner)) { break }; $seen[$owner]=$true
        $go = Read-Remote $owner 0x60
        $array = [BitConverter]::ToUInt64($go,0x30)
        $count = [BitConverter]::ToUInt64($go,0x40)
        if ($count -gt 64 -or ($count -gt 0 -and $array -eq 0)) { throw 'Owner component list exceeds bounds.' }
        $components = @(); $parent = [uint64]0
        if ($count -gt 0) {
            $list = Read-Remote $array ([int]$count*16)
            for ($i=0; $i -lt $count; $i++) {
                $type = [BitConverter]::ToUInt32($list,$i*16)
                $native = [BitConverter]::ToUInt64($list,$i*16+8)
                if ($native -eq 0) { continue }
                $item = [ordered]@{native=(Hex $native); native_type=$type}
                try {
                    $n = Read-Remote $native 0x40
                    $item.native_header_raw_hex = [BitConverter]::ToString($n).Replace('-','')
                    if ([BitConverter]::ToUInt64($n,0x30) -ne $owner) { throw 'Component owner changed.' }
                    if ($type -ge $transformMin -and ([uint64]$type-$transformMin) -lt $transformCount) {
                        # RVA 0x83a1f7..0x83a20c: Transform+0x90 -> parent Transform -> owner.
                        $pt = [BitConverter]::ToUInt64((Read-Remote ($native+0x90) 8),0)
                        if ($pt -ne 0) { $parent = [BitConverter]::ToUInt64((Read-Remote ($pt+0x30) 8),0) }
                    }
                    if ([BitConverter]::ToUInt32($n,0x20) -eq 2) {
                        $managed = [BitConverter]::ToUInt64($n,0x28)
                        $item.managed = Hex $managed
                        if ($managed -ne 0) {
                            $head = Read-Remote $managed 24
                            if ([BitConverter]::ToUInt64($head,16) -ne $native) { throw 'Managed/native back reference differs.' }
                            $klass = [BitConverter]::ToUInt64($head,0)
                            $k = Read-Remote $klass 32
                            $item.class_name = Read-AsciiName ([BitConverter]::ToUInt64($k,16))
                            $item.namespace = Read-AsciiName ([BitConverter]::ToUInt64($k,24))
                            if ($item.namespace -eq 'UiScripts' -and $item.class_name -eq 'SplashScreen') {
                                $s = Read-Remote $managed 0xa8
                                $item.splash = [ordered]@{
                                    matches_video=([BitConverter]::ToUInt64($s,0x30) -eq $videoObject);
                                    video=(Hex ([BitConverter]::ToUInt64($s,0x30)));
                                    skip_finished=$s[0x90]; on_presents_display=$s[0xa0]; on_splash_video_display=$s[0xa1];
                                    raw_hex=([BitConverter]::ToString($s).Replace('-',''))}
                            } elseif ($item.class_name -eq 'BootSplashSequenceController' -or $item.class_name -eq 'ExecuteEventOnVideoFinished') {
                                $s = Read-Remote $managed 0x60
                                $item.raw_hex = [BitConverter]::ToString($s).Replace('-','')
                            }
                        }
                    }
                } catch { $item.error=$_.Exception.Message }
                $components += $item
            }
        }
        $owners += [ordered]@{address=(Hex $owner); component_count=$count; components=$components; parent=(Hex $parent)}
        $owner=$parent
    }
    $result.owners=$owners
    try {
        # Native pointer from captured LoopPoint MethodInfo matches RVA 0x4715b70,
        # base 0x930000000. Verify code before using this module's static fields.
        $sig = Read-Remote 0x934715b70 9
        if ([BitConverter]::ToString($sig) -ne '48-85-FF-74-1B-48-8B-47-20') { throw 'Unknown managed module code/base.' }
        $klass = [BitConverter]::ToUInt64((Read-Remote 0x935e28908 8),0)
        $statics = [BitConverter]::ToUInt64((Read-Remote ($klass+0xb8) 8),0)
        $sf = Read-Remote $statics 16
        $instance = [BitConverter]::ToUInt64($sf,8)
        $fmod = [ordered]@{klass=(Hex $klass); statics=(Hex $statics);
            init_exception=(Hex ([BitConverter]::ToUInt64($sf,0))); instance=(Hex $instance)}
        if ($instance -ne 0) {
            $f = Read-Remote $instance 0xc0
            if ([BitConverter]::ToUInt64($f,0) -ne $klass) { throw 'FMOD instance class differs.' }
            $fmod.loading_banks_ref=[BitConverter]::ToInt32($f,0xa4)
            $fmod.loaded_banks=Hex ([BitConverter]::ToUInt64($f,0x60))
            $fmod.raw_hex=[BitConverter]::ToString($f).Replace('-','')
            if ($FmodDetails) { $fmod.details=Read-FmodDetails $f }
        }
        $result.fmod=$fmod
    } catch { $result.fmod_error=$_.Exception.Message }
    return $result
}
function Read-JobSlot([uint64]$Token) {
    # Same token validity/generation test as guest RVA 0x15af683..0x15af6cb.
    $generation = $Token -shr 32
    $index = $Token -band 0xffffffffL
    if ($generation -eq 0 -or $index -gt 0xfff) {
        return [ordered]@{token=(Hex $Token); pending_by_guest_test=$false}
    }
    $jobs = [BitConverter]::ToUInt64((Read-Remote 0x90207f140 8),0)
    if ($jobs -eq 0) { return [ordered]@{token=(Hex $Token); pending_by_guest_test=$false} }
    $pool = [BitConverter]::ToUInt64((Read-Remote ($jobs+0x20) 8),0)
    $slots = [BitConverter]::ToUInt64((Read-Remote ($pool+8) 8),0)
    $slot = Read-Remote ($slots+$index*64) 64
    return [ordered]@{token=(Hex $Token);
        pending_by_guest_test=([BitConverter]::ToUInt32($slot,0) -eq $generation);
        raw_hex=([BitConverter]::ToString($slot).Replace('-',''))}
}
function Read-EndEventState([byte[]]$PlayerData, [uint64]$PlayerAddress) {
    # RVA 0x15bc840 queues InvokeLoopPointReachedCallback_Internal.
    if ([BitConverter]::ToUInt64($PlayerData,0x198) -ne 0x9015bc840) {
        throw 'Unknown Unity completion callback; refusing component interpretation.'
    }
    $component = [BitConverter]::ToUInt64($PlayerData,0x1a0)
    $c = Read-Remote $component 0x130
    if ([BitConverter]::ToUInt64($c,0x40) -ne $PlayerAddress) {
        throw 'Video component no longer refers to the sampled player.'
    }
    $begin = [BitConverter]::ToUInt64($c,0xa0)
    $end = [BitConverter]::ToUInt64($c,0xa8)
    $capacity = [BitConverter]::ToUInt64($c,0xb0)
    if ($end -lt $begin -or $capacity -lt $end -or
        ($end-$begin) -gt (1024*0x130) -or (($end-$begin) % 0x130) -ne 0) {
        throw 'Event queue bounds changed or exceed limit; retry with a new sample.'
    }
    $count = [int](($end-$begin)/0x130)
    $cache = [BitConverter]::ToUInt64((Read-Remote 0x90209bd28 8),0)
    $loopMethod = [uint64]0
    if ($cache -ne 0) { $loopMethod = [BitConverter]::ToUInt64((Read-Remote ($cache+0x50) 8),0) }
    $entries = @()
    # Bound reads even if the queue grows. Capture first/last four records.
    $indices = if ($count -le 8) { @(0..($count-1)) } else { @(0..3)+@(($count-4)..($count-1)) }
    if ($count -eq 0) { $indices = @() }
    foreach ($index in $indices) {
        $entry = Read-Remote ($begin+$index*0x130) 0x130
        $method = [BitConverter]::ToUInt64($entry,0xf8)
        $entries += [ordered]@{index=$index; method=(Hex $method);
            matches_loop_point_method=($loopMethod -ne 0 -and $method -eq $loopMethod);
            raw_hex=([BitConverter]::ToString($entry).Replace('-',''))}
    }
    $details = $null
    if ($CompletionDetails) {
        $details = [ordered]@{note='Allocated slot contents are residual memory, NOT a pending event or proof of invocation.'}
        if ($begin -ne 0 -and $capacity-$begin -ge 0x130 -and
            $capacity-$begin -le (1024*0x130)) {
            $slot = Read-Remote $begin 0x130
            $details.first_allocated_slot = [ordered]@{
                method=(Hex ([BitConverter]::ToUInt64($slot,0xf8)));
                argument_0=(Hex ([BitConverter]::ToUInt64($slot,0x60)));
                raw_hex=([BitConverter]::ToString($slot).Replace('-',''))}
        }
        if ($loopMethod -ne 0) {
            $methodData = Read-Remote $loopMethod 0x80
            $details.loop_method_raw_hex = [BitConverter]::ToString($methodData).Replace('-','')
        }
        # RVA 0xcf7137: native object kind 2 stores its scripting object at +0x28.
        if ([BitConverter]::ToUInt32($c,0x20) -eq 2) {
            $managed = [BitConverter]::ToUInt64($c,0x28)
            $details.scripting_object = Hex $managed
            if ($managed -ne 0) {
                $managedData = Read-Remote $managed 0x100
                $details.scripting_object_raw_hex = [BitConverter]::ToString($managedData).Replace('-','')
            }
        }
    }
    return [ordered]@{component=(Hex $component); state_60=[BitConverter]::ToUInt32($c,0x60);
        looping_118=$c[0x118]; queue_begin=(Hex $begin); queue_end=(Hex $end);
        queue_count=$count; loop_point_method=(Hex $loopMethod); entries=$entries; completion_details=$details;
        startup_state=$(if ($StartupState) { Read-StartupState $c } else { $null });
        raw_hex=([BitConverter]::ToString($c).Replace('-',''))}
}
function Read-PlaybackState([uint64]$Backend) {
    # Version-specific addresses derived statically from this eboot; no guest calls.
    # P is the upper Unity player, distinct from the Av callback object (Backend).
    $manager = [BitConverter]::ToUInt64((Read-Remote 0x9020d67d8 8),0)
    if ($manager -eq 0) { throw 'Unity video manager is null.' }
    $header = Read-Remote $manager 0x98
    $players = @{}
    $lists = [ordered]@{}
    foreach ($offset in @(0x20,0x80)) {
        $array = [BitConverter]::ToUInt64($header,$offset)
        $count = [BitConverter]::ToUInt64($header,$offset+0x10)
        $lists[(Hex $offset)] = $count
        if ($count -gt 64) { throw 'Unity player list exceeds read limit (64).' }
        if ($count -eq 0) { continue }
        if ($array -eq 0) { throw 'Nonempty Unity player list has a null pointer.' }
        $entries = Read-Remote $array ([int]$count*8)
        for ($i=0; $i -lt $count; $i++) {
            $address = [BitConverter]::ToUInt64($entries,$i*8)
            if ($address -ne 0) { $players[(Hex $address)] = $address }
        }
    }
    $found = 0
    foreach ($address in $players.Values) {
        if ([BitConverter]::ToUInt64((Read-Remote $address 8),0) -ne 0x901e07488) { continue }
        $p = Read-Remote $address 0x1f8
        if ([BitConverter]::ToUInt64($p,0xe0) -ne $Backend) { continue }
        $found++
        $times = [ordered]@{}
        foreach ($offset in @(0x80,0x158,0x160,0x168,0x170,0x1d0,0x1d8)) {
            $times[(Hex $offset)] = [BitConverter]::ToDouble($p,$offset)
        }
        $helper = [BitConverter]::ToUInt64($p,0xe8)
        $helperData = $null
        if ($helper -ne 0) {
            $vt = [BitConverter]::ToUInt64((Read-Remote $helper 8),0)
            $length = if ($vt -eq 0x901e12970) { 0x50 } elseif ($vt -eq 0x901e128b8) { 0xa0 } else { 8 }
            $h = Read-Remote $helper $length
            $helperData = [ordered]@{address=(Hex $helper); vtable=(Hex $vt);
                raw_hex=([BitConverter]::ToString($h).Replace('-',''))}
        }
        $clock = [BitConverter]::ToUInt64($p,0x150)
        $clockData = $null
        if ($clock -ne 0) {
            $vt = [BitConverter]::ToUInt64((Read-Remote $clock 8),0)
            # Only the statically verified default clock has a known layout here.
            $length = if ($vt -eq 0x901e07630) { 0x20 } else { 8 }
            $c = Read-Remote $clock $length
            $clockData = [ordered]@{address=(Hex $clock); vtable=(Hex $vt);
                raw_hex=([BitConverter]::ToString($c).Replace('-',''))}
            if ($vt -eq 0x901e07630 -or $vt -eq 0x901e073a0) {
                $engine = [BitConverter]::ToUInt64((Read-Remote 0x9020380e8 8),0)
                $clockData.engine_address = Hex $engine
                if ($engine -ne 0) {
                    $e = Read-Remote ($engine+0x90) 0x70
                    $clockData.engine_time_90 = [BitConverter]::ToDouble($e,0)
                    $clockData.engine_time_a0 = [BitConverter]::ToDouble($e,0x10)
                    $clockData.engine_counter_c8 = [BitConverter]::ToInt64($e,0x38)
                    $clockData.engine_float_d8 = [BitConverter]::ToSingle($e,0x48)
                    $clockData.engine_float_fc = [BitConverter]::ToSingle($e,0x6c)
                    $clockData.engine_90_to_ff_raw_hex = [BitConverter]::ToString($e).Replace('-','')
                }
            }
        }
        try { $endEvent = Read-EndEventState $p $address }
        catch { $endEvent = @{error=$_.Exception.Message} }
        $jobData = [ordered]@{}
        foreach ($offset in @(0x90,0x1e0)) {
            try { $jobData[(Hex $offset)] = Read-JobSlot ([BitConverter]::ToUInt64($p,$offset)) }
            catch { $jobData[(Hex $offset)] = @{error=$_.Exception.Message} }
        }
        [ordered]@{kind='playback'; utc=[DateTime]::UtcNow.ToString('o');
            manager=(Hex $manager); list_counts=$lists; player=(Hex $address);
            backend=(Hex $Backend); active_byte_7c=$p[0x7c]; ack_request_88=$p[0x88];
            looping_byte_184=$p[0x184]; time_mode_byte_18c=$p[0x18c];
            frame_60=[BitConverter]::ToInt64($p,0x60);
            clock_mode_120=[BitConverter]::ToUInt32($p,0x120); times=$times;
            ack_job_90=(Hex ([BitConverter]::ToUInt64($p,0x90)));
            ack_job_generation_98=[BitConverter]::ToUInt32($p,0x98);
            dependency_1e0=(Hex ([BitConverter]::ToUInt64($p,0x1e0)));
            dependency_generation_1e8=[BitConverter]::ToUInt32($p,0x1e8);
            player_generation_1f0=[BitConverter]::ToUInt32($p,0x1f0);
            helper=$helperData; clock=$clockData; jobs=$jobData; end_event=$endEvent;
            raw_hex=([BitConverter]::ToString($p).Replace('-',''))} | ConvertTo-Json -Compress -Depth 16
    }
    if ($found -eq 0) { throw 'Matching upper Unity player not found in bounded manager lists.' }
}
try {
    $signature = Read-Remote 0x90155ba43 19
    if ([BitConverter]::ToString($signature) -ne '83-FE-20-0F-85-C0-00-00-00-8B-31-81-FE-04-00-6A-80-0F-94') {
        throw 'Callback signature differs from inspected game version; refusing field interpretation.'
    }
    [ordered]@{kind='identity'; pid=$TargetPid; start_utc=$started;
        object=(Hex $ObjectAddress); callback='0x90155ba20'; access='0x1010 read only';
        note='Live non-atomic samples. Unknown offsets have no assigned semantics.'} | ConvertTo-Json -Compress
    for ($sample=0; $sample -lt $Samples; $sample++) {
        $process.Refresh()
        if ($process.HasExited) { throw 'Process exited.' }
        $data = Read-Remote $ObjectAddress 0x6b0
        if ([BitConverter]::ToUInt64($data,0) -ne 0x901e06670) {
            throw 'Consumer vtable differs; object may be stale or game version may differ.'
        }
        $words = [ordered]@{}
        foreach ($offset in @(0x18,0x1c,0x20,0x24,0x30,0x34,0x50,0x54,0x690,0x6a0)) {
            $words[(Hex $offset)] = Hex ([BitConverter]::ToUInt32($data,$offset))
        }
        $quads = [ordered]@{}
        foreach ($offset in @(0,0x28,0x38,0x40,0x48,0x50,0x58,0x60,0x5a0,
                              0x5e0,0x5e8,0x5f0,0x5f8,0x600,0x608,0x610,0x618,
                              0x620,0x628,0x630,0x638,0x640,0x648,0x650,0x658,
                              0x660,0x668,0x670,0x678,0x680,0x688,0x698)) {
            $quads[(Hex $offset)] = Hex ([BitConverter]::ToUInt64($data,$offset))
        }
        [ordered]@{kind='sample'; utc=[DateTime]::UtcNow.ToString('o'); index=$sample;
            expected_code=(Hex ([BitConverter]::ToUInt32($data,0x690)));
            video_gate_count=[BitConverter]::ToInt32($data,0x50);
            duration_ms=[BitConverter]::ToUInt64($data,0x40);
            high_water_timestamp_ms=[BitConverter]::ToUInt64($data,0x48);
            retained_timestamp_ms=[BitConverter]::ToUInt64($data,0x5e0);
            wrap_seen_byte=$data[0x33]; retained_frame_flag=$data[0x608];
            dwords=$words; qwords=$quads;
            raw_hex=([BitConverter]::ToString($data).Replace('-',''))} | ConvertTo-Json -Compress -Depth 4
        if ($PlaybackState) {
            try { Read-PlaybackState $ObjectAddress }
            catch {
                [ordered]@{kind='playback_error'; utc=[DateTime]::UtcNow.ToString('o');
                    error=$_.Exception.Message} | ConvertTo-Json -Compress
            }
        }
        if ($sample+1 -lt $Samples) { Start-Sleep -Milliseconds 1000 }
    }
    if ($ReadAddress -ne 0) {
        $extra = Read-Remote $ReadAddress $ReadLength
        [ordered]@{kind='extra'; address=(Hex $ReadAddress); length=$ReadLength;
            raw_hex=([BitConverter]::ToString($extra).Replace('-',''))} | ConvertTo-Json -Compress
    }
} finally {
    [void][SpartaReadOnly]::CloseHandle($handle)
}
