# order-endpoints.ps1 - put the Apollo sound devices in a fixed order everywhere Windows lists them.
#
# Windows lists sound devices (Sound control panel, Settings, the volume flyout, app device pickers) sorted
# by endpoint ID, {0.0.0.00000000}.{GUID} for playback and {0.0.1.00000000}.{GUID} for recording, and the
# GUID is random (version 4), made by the Windows Audio Endpoint Builder the first time it sees a device.
# So the Apollo devices come out shuffled between everything else (verified Nov 15 2026: enumeration order
# == GUID order).
#
# This gives every present Apollo endpoint an ID that sorts by unit and jack:
#   {00000000-a90e-4a90-8a90-0000000000NN}   NN = 00, 01, ... in this order:
#   playback : Monitor, then x8p MON L/R, LINE 1-2 .. 7-8, HP 1, HP 2, S/PDIF, then the Twin X's
#   recording: Monitor mix, then x8p MIC/LINE 1-8, VIRTUAL, S/PDIF, then the Twin X's
# so they sort ahead of every other device, in that order.
#
# How (the audio services stopped for a few seconds):
#   1. remove each endpoint's software device SWD\MMDEVAPI\<old id> (the Endpoint Builder restores the old
#      key from it otherwise: first try, Nov 15 2026);
#   2. rename its MMDevices key to the new GUID (the default-device roles live in that key and move with it);
#   3. rewrite every reference to the old ID in every endpoint's Properties (each playback endpoint carries
#      the default's ID; each endpoint caches its own SWD path);
#   4. start the services: the builder makes new software devices under the new IDs.
# The keys are writable only by the Endpoint Builder service and TrustedInstaller (not even SYSTEM), so the
# writes go through handles opened with REG_OPTION_BACKUP_RESTORE under SeRestorePrivilege: no ACL is changed.
# Run in an ADMIN PowerShell. Re-run after an install that creates new Apollo endpoints (new reference
# strings, a new endpoint set): it only moves endpoints that are not in place yet.
#
#   -DryRun   print the plan only (nothing stopped, nothing written)
# PowerShell 5.1, ASCII only.
param(
    [switch]$DryRun,
    [string]$Log = ''
)
$ErrorActionPreference = 'Continue'
# DISABLED (Nov 15 2026): Windows refuses to rename the endpoint keys, for SYSTEM and for backup/restore handles
# alike (RegRenameKey and NtRenameKey: access denied), and the Endpoint Builder restores an endpoint's old ID
# from its software device. Kept as the record of what was tried; it stops before changing anything.
if (-not $DryRun) {
    Write-Host 'order-endpoints is disabled: Windows does not let the endpoint IDs be renamed (see the header). Nothing changed.'
    exit 5
}
if (-not $Log) { $Log = Join-Path $PSScriptRoot 'order-endpoints.log' }
"order-endpoints $(Get-Date -Format o) DryRun=$DryRun user=$([Security.Principal.WindowsIdentity]::GetCurrent().Name)" | Out-File -Encoding ascii $Log
function Note([string]$s) { Write-Host $s; Add-Content -Encoding ascii $Log $s }

$mmd = 'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio'
$pkDevPath = '{b3f8fa53-0004-438e-9003-51a46e139bfc},2'   # "{n}.<parent device instance>"
$pkPinName = '{a45c254e-df1c-4efd-8020-67d146a850e0},2'   # the endpoint name ("Monitor", "x8p HP 1")

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace AviumOrder {
    public static class Reg {
        const int REG_OPTION_BACKUP_RESTORE = 4;
        const int KEY_ALL_ACCESS = 0xF003F;
        const int REG_SZ = 1;
        static readonly IntPtr HKLM = new IntPtr(unchecked((int)0x80000002));

        [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
        static extern int RegOpenKeyExW(IntPtr hKey, string sub, int options, int sam, out IntPtr result);
        [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
        static extern int RegRenameKey(IntPtr hKey, string sub, string newName);
        [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
        static extern int RegSetValueExW(IntPtr hKey, string name, int reserved, int type, byte[] data, int cb);
        [DllImport("advapi32.dll")]
        static extern int RegCloseKey(IntPtr hKey);

        [StructLayout(LayoutKind.Sequential, Pack = 4)]
        struct TP { public int Count; public long Luid; public int Attr; }
        [DllImport("advapi32.dll", SetLastError = true)]
        static extern bool OpenProcessToken(IntPtr h, int acc, out IntPtr tok);
        [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern bool LookupPrivilegeValueW(string sys, string name, out long luid);
        [DllImport("advapi32.dll", SetLastError = true)]
        static extern bool AdjustTokenPrivileges(IntPtr tok, bool all, ref TP tp, int len, IntPtr prev, IntPtr rlen);
        [DllImport("kernel32.dll")]
        static extern IntPtr GetCurrentProcess();

        /* 0 = enabled; else the Win32 error (1300 = the token does not hold it: not elevated) */
        public static int Enable(string name) {
            IntPtr tok; long luid;
            if (!OpenProcessToken(GetCurrentProcess(), 0x28, out tok)) return Marshal.GetLastWin32Error();
            if (!LookupPrivilegeValueW(null, name, out luid)) return Marshal.GetLastWin32Error();
            TP tp = new TP(); tp.Count = 1; tp.Luid = luid; tp.Attr = 2;
            AdjustTokenPrivileges(tok, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero);
            return Marshal.GetLastWin32Error();
        }
        static int Open(string path, out IntPtr h) {
            return RegOpenKeyExW(HKLM, path, REG_OPTION_BACKUP_RESTORE, KEY_ALL_ACCESS, out h);
        }
        /* rename HKLM\path (the key itself) to newName (a sibling name, no backslash) */
        public static int Rename(string path, string newName) {
            IntPtr h; int rc = Open(path, out h);
            if (rc != 0) return rc;
            rc = RegRenameKey(h, null, newName);
            RegCloseKey(h);
            return rc;
        }
        public static int SetString(string path, string name, string value) {
            IntPtr h; int rc = Open(path, out h);
            if (rc != 0) return rc;
            byte[] b = System.Text.Encoding.Unicode.GetBytes(value + "\0");
            rc = RegSetValueExW(h, name, 0, REG_SZ, b, b.Length);
            RegCloseKey(h);
            return rc;
        }
    }
}
'@

# ---- the Apollo units (present or not): instance id -> model
$units = @{}
foreach ($d in @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match '^PCI\\VEN_1A00&DEV_0002&SUBSYS_00(14|19)1A00&' })) {
    $units[$d.InstanceId.ToUpperInvariant()] = $(if ($d.InstanceId -match 'SUBSYS_0014') { 'x8p' } else { 'twinx' })
}
if ($units.Count -eq 0) { Note 'No Apollo unit known to Windows: nothing to order.'; exit 1 }

$renderOrder  = @('Monitor', 'MON L/R', 'LINE 1-2', 'LINE 3-4', 'LINE 5-6', 'LINE 7-8', 'HP 1', 'HP 2', 'HP', 'S/PDIF')
$captureOrder = @('Monitor mix', 'MON L/R', 'MIC/LINE 1', 'MIC/LINE 2', 'MIC/LINE 3', 'MIC/LINE 4', 'MIC/LINE 5',
                  'MIC/LINE 6', 'MIC/LINE 7', 'MIC/LINE 8', 'VIRTUAL 1-2', 'VIRTUAL 3-4', 'VIRTUAL 5-6',
                  'VIRTUAL 7-8', 'S/PDIF')
function Rank([string]$name, [string]$model, [string[]]$order) {
    $unit = $(if ($model -eq 'x8p') { 1 } else { 2 })
    $pin = $name
    if ($name -match '^x8p (.*)$') { $unit = 1; $pin = $Matches[1] }
    elseif ($name -match '^Twin X (.*)$') { $unit = 2; $pin = $Matches[1] }
    elseif ($name -eq 'Monitor' -or $name -eq 'Monitor mix') { $unit = 0 }
    $i = [array]::IndexOf($order, $pin)
    if ($i -lt 0) { $i = 90 }
    return ('{0:D1}{1:D2}|{2}' -f $unit, $i, $name)
}
function Target([int]$i) { return ('{{00000000-a90e-4a90-8a90-0000000000{0}}}' -f $i.ToString('x2')) }

# ---- the plan, per flow
$plans = @()
foreach ($flow in @(@{ Name = 'Render'; Order = $renderOrder; Prefix = '{0.0.0.00000000}' },
                    @{ Name = 'Capture'; Order = $captureOrder; Prefix = '{0.0.1.00000000}' })) {
    $root = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey("$mmd\$($flow.Name)", $false)
    $all = @($root.GetSubKeyNames())
    $rows = @()
    foreach ($sub in $all) {
        $k = $root.OpenSubKey($sub, $false)
        $state = [int]$k.GetValue('DeviceState', 0)
        $p = $k.OpenSubKey('Properties', $false)
        $dev = ''; $name = ''
        if ($p) { $dev = [string]$p.GetValue($pkDevPath, ''); $name = [string]$p.GetValue($pkPinName, ''); $p.Close() }
        $k.Close()
        $inst = ($dev -replace '^\{\d+\}\.', '').ToUpperInvariant()
        if (-not $units.ContainsKey($inst)) { continue }
        $rows += [pscustomobject]@{ Key = $sub; Name = $name; Model = $units[$inst]; State = $state;
                                    Present = (($state -band 4) -eq 0); Rank = (Rank $name $units[$inst] $flow.Order) }
    }
    $root.Close()
    $present = @($rows | Where-Object Present | Sort-Object Rank)
    $i = 0
    foreach ($r in $present) { $r | Add-Member Target (Target $i); $i++ }
    $plans += [pscustomobject]@{ Flow = $flow.Name; Prefix = $flow.Prefix; Present = $present; All = $all;
                                 Moving = @($present | Where-Object { $_.Key -ine $_.Target }) }
}

$todo = 0
foreach ($pl in $plans) {
    Note "--- $($pl.Flow): $($pl.Present.Count) present Apollo endpoint(s)"
    foreach ($r in $pl.Present) {
        Note ("  {0,-26} {1} -> {2}{3}" -f $r.Name, $r.Key, $r.Target, $(if ($r.Key -ieq $r.Target) { ' (already)' } else { '' }))
    }
    $todo += $pl.Moving.Count
}
if ($todo -eq 0) { Note 'Already in order: nothing to do.'; exit 0 }
if ($DryRun) { Note "Dry run: $todo endpoint(s) would get a new ID."; exit 0 }

foreach ($priv in 'SeRestorePrivilege', 'SeBackupPrivilege') {
    $rc = [AviumOrder.Reg]::Enable($priv)
    if ($rc -ne 0) { Note "Could not enable $priv (error $rc): run this in an ADMIN PowerShell. Nothing changed."; exit 2 }
}

# ---- stop the audio services (Audiosrv depends on AudioEndpointBuilder), remember what ran
$dependents = @(@(Get-Service AudioEndpointBuilder -ErrorAction SilentlyContinue).DependentServices | Where-Object { $_.Status -eq 'Running' })
Note "Stopping AudioEndpointBuilder (and running dependents: $(($dependents | ForEach-Object Name) -join ', '))"
Stop-Service -Name AudioEndpointBuilder -Force -ErrorAction SilentlyContinue
$deadline = (Get-Date).AddSeconds(30)
while ((Get-Date) -lt $deadline -and (Get-Service AudioEndpointBuilder).Status -ne 'Stopped') { Start-Sleep -Milliseconds 300 }
if ((Get-Service AudioEndpointBuilder).Status -ne 'Stopped') {
    Note 'AudioEndpointBuilder did not stop: nothing changed.'
    Start-Service Audiosrv -ErrorAction SilentlyContinue
    exit 3
}

$failed = 0
$idMap = @{}
try {
    $pnputil = Join-Path $env:SystemRoot 'System32\pnputil.exe'
    foreach ($pl in $plans) {
        $base = "$mmd\$($pl.Flow)"
        # a target name held by a key that is not moving into it (left from an earlier run): park it
        foreach ($r in $pl.Moving) {
            $holder = $pl.All | Where-Object { $_ -ieq $r.Target } | Select-Object -First 1
            if ($holder -and -not (@($pl.Moving | Where-Object { $_.Key -ieq $r.Target }).Count)) {
                $park = '{' + [guid]::NewGuid().ToString() + '}'
                $rc = [AviumOrder.Reg]::Rename("$base\$holder", $park)
                Note ("  parked {0} -> {1}: {2}" -f $holder, $park, $rc)
            }
        }
        # 1. the software devices of the old IDs
        foreach ($r in $pl.Moving) {
            $swd = "SWD\MMDEVAPI\$($pl.Prefix).$($r.Key)"
            [void](& $pnputil /remove-device $swd 2>&1 | Out-String)
            Note ("  remove-device {0}: exit {1}" -f $swd, $LASTEXITCODE)
        }
        # 2. the keys, in two phases so a target never collides with a key still to move
        foreach ($r in $pl.Moving) {
            $tmp = '{' + [guid]::NewGuid().ToString() + '}'
            $rc = [AviumOrder.Reg]::Rename("$base\$($r.Key)", $tmp)
            if ($rc -eq 0) { $r | Add-Member Tmp $tmp } else { $failed++; Note ("  rename {0} -> tmp FAILED {1}" -f $r.Key, $rc) }
        }
        foreach ($r in $pl.Moving) {
            if (-not $r.Tmp) { continue }
            $rc = [AviumOrder.Reg]::Rename("$base\$($r.Tmp)", $r.Target)
            if ($rc -eq 0) {
                $idMap[$r.Key.Trim('{}')] = $r.Target.Trim('{}')
                Note ("  {0,-26} now {1}" -f $r.Name, $r.Target)
            } else {
                $failed++
                Note ("  rename {0} -> {1} FAILED {2}; back to {3}: {4}" -f $r.Tmp, $r.Target, $rc, $r.Key,
                      [AviumOrder.Reg]::Rename("$base\$($r.Tmp)", $r.Key))
            }
        }
    }
    # 3. every reference to an old ID, in every endpoint of both flows
    $fixed = 0
    foreach ($fl in 'Render', 'Capture') {
        $root = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey("$mmd\$fl", $false)
        foreach ($sub in $root.GetSubKeyNames()) {
            $p = $root.OpenSubKey("$sub\Properties", $false)
            if (-not $p) { continue }
            foreach ($v in $p.GetValueNames()) {
                if ($p.GetValueKind($v) -ne [Microsoft.Win32.RegistryValueKind]::String) { continue }
                $s = [string]$p.GetValue($v); $t = $s
                foreach ($o in $idMap.Keys) { $t = $t -ireplace [regex]::Escape($o), $idMap[$o] }
                if ($t -ne $s) {
                    $rc = [AviumOrder.Reg]::SetString("$mmd\$fl\$sub\Properties", $v, $t)
                    if ($rc -eq 0) { $fixed++ } else { $failed++; Note ("  rewrite {0} {1} FAILED {2}" -f $sub, $v, $rc) }
                }
            }
            $p.Close()
        }
        $root.Close()
    }
    Note "  $fixed reference(s) rewritten to the new IDs"
} catch {
    $failed++
    Note "ERROR: $($_.Exception.Message) (line $($_.InvocationInfo.ScriptLineNumber))"
} finally {
    Note 'Starting the audio services again.'
    Start-Service AudioEndpointBuilder -ErrorAction SilentlyContinue
    Start-Service Audiosrv -ErrorAction SilentlyContinue
    foreach ($d in $dependents) { Start-Service -Name $d.Name -ErrorAction SilentlyContinue }
    Note ("AudioEndpointBuilder {0}, Audiosrv {1}" -f (Get-Service AudioEndpointBuilder).Status, (Get-Service Audiosrv).Status)
}
Note "order-endpoints done: $todo planned, $($idMap.Count) moved, $failed failure(s). Then clean-endpoints.ps1 removes leftovers."
exit $(if ($failed -eq 0) { 0 } else { 4 })
