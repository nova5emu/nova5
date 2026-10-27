# Offline parser checks only. No process handles or game data required.
$ErrorActionPreference = 'Stop'
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot 'InspectSpartaConsumer.ps1'),[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
foreach ($name in @('Hex','Read-ManagedString','Read-ManagedStringList','Read-FmodDetails')) {
    $fn=$ast.Find({param($a) $a -is [Management.Automation.Language.FunctionDefinitionAst] -and $a.Name -eq $name},$true)
    Invoke-Expression $fn.Extent.Text
}
$mem=@{}; $script:mutateHeader=$false; $script:dictReads=0
function Put64($b,$o,[uint64]$v) { [BitConverter]::GetBytes($v).CopyTo($b,$o) }
function Put32($b,$o,[int]$v) { [BitConverter]::GetBytes($v).CopyTo($b,$o) }
function Save([uint64]$a,[byte[]]$b) { $mem[$a.ToString()]=$b }
function Read-Remote([uint64]$Address,[int]$Length) {
    if ($Address -eq 0x1000) {
        $script:dictReads++
        if ($script:mutateHeader -and $script:dictReads -eq 2) { Put32 $mem['4096'] 0x2c 9 }
    }
    if ($Length -gt 8192) { throw 'Unexpected oversized read' }
    foreach ($key in $mem.Keys) {
        $base=[uint64]$key; $data=$mem[$key]
        if ($Address -ge $base -and $Address+$Length -le $base+$data.Length) {
            $copy=[byte[]]::new($Length)
            [Array]::Copy($data,[int]($Address-$base),$copy,0,$Length)
            return ,$copy
        }
    }
    throw ('Unexpected mock read 0x{0:x}/{1}' -f $Address,$Length)
}
function Assert($condition,$message) { if (-not $condition) { throw $message } }
$instance=[byte[]]::new(0xc0); Put64 $instance 0x60 0x1000; Put64 $instance 0x68 0x4000
$dict=[byte[]]::new(0x30); Put64 $dict 0x18 0x2000; Put32 $dict 0x20 2; Save 0x1000 $dict
$entries=[byte[]]::new(96); Put64 $entries 24 2; Put32 $entries 32 123
Put64 $entries 40 0x3000; Put64 $entries 48 0x123456; Put32 $entries 56 3
Put32 $entries 64 -1; Put64 $entries 72 0xdeadbeefL; Save 0x2000 $entries
$s=[byte[]]::new(40); Put32 $s 16 7
$bankName='M'+[char]0xfa+'sica'+[char]0xd83d+[char]0xde00
Put32 $s 16 $bankName.Length
[Text.Encoding]::Unicode.GetBytes($bankName).CopyTo($s,20); Save 0x3000 $s
$list=[byte[]]::new(32); Put64 $list 16 0x5000; Put32 $list 24 1; Save 0x4000 $list
$items=[byte[]]::new(40); Put64 $items 24 1; Put64 $items 32 0x3000; Save 0x5000 $items
# Settings and its configured lists; all bindings unresolved.
$ptr=[byte[]]::new(8); Put64 $ptr 0 0x6000; Save 0x935e28fe0 $ptr
$ptr=[byte[]]::new(8); Put64 $ptr 0 0x7000; Save 0x60b8 $ptr
$ptr=[byte[]]::new(8); Put64 $ptr 0 0x8000; Save 0x7000 $ptr
$settings=[byte[]]::new(0xc8); Put64 $settings 0 0x6000
Put64 $settings 0x58 0x3000; Put64 $settings 0x60 0x3000
Put64 $settings 0xb0 0x4000; Put64 $settings 0xb8 0x4000; Put64 $settings 0xc0 0x4000
Save 0x8000 $settings
foreach ($a in @(0x93627e600L,0x93627e9e0L,0x93627e9f8L)) { Save $a ([byte[]]::new(8)) }
$r=Read-FmodDetails $instance
Assert ($r.registered_banks.entries.Count -eq 1) 'Deleted dictionary entry was not skipped'
Assert ($r.registered_banks.entries[0].name -eq $bankName) 'UTF16 string decoded incorrectly'
Assert ($r.registered_banks.entries[0].bank_handle -eq '0x123456') 'Opaque handle changed'
Assert ($r.sample_load_requests.items[0].text -eq $bankName) 'Sample request name missing'
Assert ($r.settings.target_bank_folder -eq $bankName) 'Settings path missing'
Assert ($r.settings.banks_to_load.items[0].text -eq $bankName) 'Configured bank list missing'
Assert $r.registered_banks.header_stable 'Stable dictionary marked changed'
$script:dictReads=0; $script:mutateHeader=$true
$r=Read-FmodDetails $instance
Assert (-not $r.registered_banks.header_stable) 'Concurrent version change went undetected'
$script:mutateHeader=$false
Put32 $dict 0x20 257
$r=Read-FmodDetails $instance
Assert ($r.banks_error -eq 'Bank dictionary exceeds bounds.') 'Dictionary bound not enforced'
Assert ($r.sample_load_requests.size -eq 1) 'One failed section hid remaining diagnostics'
Put32 $list 24 2
$r=Read-FmodDetails $instance
Assert ($r.sample_requests_error -eq 'String list size exceeds capacity.') 'Array bound not enforced'
Put32 $s 16 1025
try { Read-ManagedString 0x3000; throw 'Unbounded string accepted' }
catch { Assert ($_.Exception.Message -eq 'Managed string exceeds bounds.') 'Wrong string error' }
'PASS: bank names, deleted slots, opaque handles, sample queue, concurrent mutation and read bounds (offline).'
