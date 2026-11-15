# Windows audio helpers for install.ps1 / revert.ps1. Dot-source it:  . (Join-Path $PSScriptRoot 'audio-helpers.ps1')
# PowerShell 5.1, ASCII only. Core Audio through COM interop (C# 5, compiled once per session by Add-Type).
#   Get-AudioEndpoints                 active playback (Flow 0) and recording (Flow 1) endpoints
#   Get-AudioDefaults                  default endpoint per flow and role (Console, Multimedia, Communications)
#   Restore-AudioDefaults $saved       put those defaults back where Windows moved them (IPolicyConfig)
#   Get-ApolloAudioSessions [-Filter]  active audio sessions on "(Apollo" endpoints (or -Filter's), with process names
#   Test-ApolloInUse $diag             everything that holds the x8p: processes, ACX streams, ASIO, sessions
#   Wait-ApolloIdle $diag [-Yes]       ask the user to close those, wait up to 60 s, then confirm
# SetDefaultEndpoint goes through IPolicyConfig, an undocumented but long-stable Windows interface (the one
# Sound settings uses). If it ever fails, the manual fix is: mmsys.cpl -> Playback -> right-click your
# speakers -> Set as Default Device (and Set as Default Communication Device).

if (-not ('AviumAudio.Audio' -as [type])) {
# Add-Type compiles into TEMP. Elevated, that must be the admin-only folder safe-tools.ps1 sets up, or a
# user-level process could swap the compiled assembly before this elevated session loads it.
$isAdmin = (New-Object System.Security.Principal.WindowsPrincipal([System.Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)
if ($isAdmin -and -not ((Get-Command Test-SafeTemp -ErrorAction SilentlyContinue) -and (Test-SafeTemp))) {
    throw 'audio-helpers.ps1 runs elevated but TEMP is not the admin-only folder: dot-source safe-tools.ps1 and call Initialize-SafeTools first.'
}
Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace AviumAudio {
    [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class MMDeviceEnumeratorCo { }
    [ComImport, Guid("870AF99C-171D-4F9E-AF0D-E63DF40C2BC9")] class PolicyConfigCo { }

    [StructLayout(LayoutKind.Sequential)]
    struct PROPERTYKEY { public Guid fmtid; public int pid; }

    [StructLayout(LayoutKind.Explicit, Size = 24)]
    struct PROPVARIANT {
        [FieldOffset(0)] public ushort vt;
        [FieldOffset(8)] public IntPtr p;
    }

    [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDeviceEnumerator {
        [PreserveSig] int EnumAudioEndpoints(int flow, int stateMask, out IMMDeviceCollection devices);
        [PreserveSig] int GetDefaultAudioEndpoint(int flow, int role, out IMMDevice device);
        [PreserveSig] int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, out IMMDevice device);
        [PreserveSig] int RegisterEndpointNotificationCallback(IntPtr client);
        [PreserveSig] int UnregisterEndpointNotificationCallback(IntPtr client);
    }

    [ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDeviceCollection {
        [PreserveSig] int GetCount(out uint count);
        [PreserveSig] int Item(uint index, out IMMDevice device);
    }

    [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDevice {
        [PreserveSig] int Activate(ref Guid iid, int clsCtx, IntPtr activationParams,
                                   [MarshalAs(UnmanagedType.IUnknown)] out object iface);
        [PreserveSig] int OpenPropertyStore(int access, out IPropertyStore store);
        [PreserveSig] int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
        [PreserveSig] int GetState(out int state);
    }

    [ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IPropertyStore {
        [PreserveSig] int GetCount(out uint count);
        [PreserveSig] int GetAt(uint index, out PROPERTYKEY key);
        [PreserveSig] int GetValue(ref PROPERTYKEY key, out PROPVARIANT value);
        [PreserveSig] int SetValue(ref PROPERTYKEY key, ref PROPVARIANT value);
        [PreserveSig] int Commit();
    }

    [ComImport, Guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioSessionManager2 {
        [PreserveSig] int GetAudioSessionControl(IntPtr sessionGuid, int flags, out IntPtr control);
        [PreserveSig] int GetSimpleAudioVolume(IntPtr sessionGuid, int flags, out IntPtr volume);
        [PreserveSig] int GetSessionEnumerator(out IAudioSessionEnumerator sessions);
        [PreserveSig] int RegisterSessionNotification(IntPtr n);
        [PreserveSig] int UnregisterSessionNotification(IntPtr n);
        [PreserveSig] int RegisterDuckNotification(IntPtr id, IntPtr n);
        [PreserveSig] int UnregisterDuckNotification(IntPtr n);
    }

    [ComImport, Guid("E2F5BB11-0570-40CA-ACDD-3AA01277DEE8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioSessionEnumerator {
        [PreserveSig] int GetCount(out int count);
        [PreserveSig] int GetSession(int index, [MarshalAs(UnmanagedType.IUnknown)] out object session);
    }

    /* IAudioSessionControl's methods first, then IAudioSessionControl2's (one vtable). */
    [ComImport, Guid("BFB7FF88-7239-4FC9-8FA2-07C950BE9C6D"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioSessionControl2 {
        [PreserveSig] int GetState(out int state);
        [PreserveSig] int GetDisplayName(out IntPtr name);
        [PreserveSig] int SetDisplayName(IntPtr name, IntPtr context);
        [PreserveSig] int GetIconPath(out IntPtr path);
        [PreserveSig] int SetIconPath(IntPtr path, IntPtr context);
        [PreserveSig] int GetGroupingParam(out Guid grouping);
        [PreserveSig] int SetGroupingParam(IntPtr grouping, IntPtr context);
        [PreserveSig] int RegisterAudioSessionNotification(IntPtr n);
        [PreserveSig] int UnregisterAudioSessionNotification(IntPtr n);
        [PreserveSig] int GetSessionIdentifier(out IntPtr id);
        [PreserveSig] int GetSessionInstanceIdentifier(out IntPtr id);
        [PreserveSig] int GetProcessId(out uint pid);
        [PreserveSig] int IsSystemSoundsSession();
        [PreserveSig] int SetDuckingPreference(int optOut);
    }

    /* Undocumented; the vtable Windows 7..11 ships. Only SetDefaultEndpoint is used. */
    [ComImport, Guid("F8679F50-850A-41CF-9C72-430F290290C8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IPolicyConfig {
        [PreserveSig] int GetMixFormat(IntPtr id, IntPtr format);
        [PreserveSig] int GetDeviceFormat(IntPtr id, int def, IntPtr format);
        [PreserveSig] int ResetDeviceFormat(IntPtr id);
        [PreserveSig] int SetDeviceFormat(IntPtr id, IntPtr endpointFormat, IntPtr mixFormat);
        [PreserveSig] int GetProcessingPeriod(IntPtr id, int def, IntPtr defPeriod, IntPtr minPeriod);
        [PreserveSig] int SetProcessingPeriod(IntPtr id, IntPtr period);
        [PreserveSig] int GetShareMode(IntPtr id, IntPtr mode);
        [PreserveSig] int SetShareMode(IntPtr id, IntPtr mode);
        [PreserveSig] int GetPropertyValue(IntPtr id, int fxStore, IntPtr key, IntPtr value);
        [PreserveSig] int SetPropertyValue(IntPtr id, int fxStore, IntPtr key, IntPtr value);
        [PreserveSig] int SetDefaultEndpoint([MarshalAs(UnmanagedType.LPWStr)] string id, int role);
        [PreserveSig] int SetEndpointVisibility(IntPtr id, int visible);
    }

    public class Endpoint {
        public int Flow;        /* 0 playback, 1 recording */
        public string Id;
        public string Name;
    }

    public class Session {
        public int Flow;
        public string Endpoint;
        public uint ProcessId;
        public bool SystemSounds;
    }

    public static class Audio {
        [DllImport("ole32.dll")] static extern int PropVariantClear(ref PROPVARIANT pv);

        static IMMDeviceEnumerator Enumerator() {
            return (IMMDeviceEnumerator)new MMDeviceEnumeratorCo();
        }

        static string Name(IMMDevice d) {
            IPropertyStore ps;
            if (d.OpenPropertyStore(0, out ps) != 0 || ps == null) return "";
            PROPERTYKEY k = new PROPERTYKEY();
            k.fmtid = new Guid("A45C254E-DF1C-4EFD-8020-67D146A850E0");     /* PKEY_Device_FriendlyName */
            k.pid = 14;
            PROPVARIANT v;
            string s = "";
            if (ps.GetValue(ref k, out v) == 0) {
                if (v.vt == 31 && v.p != IntPtr.Zero) s = Marshal.PtrToStringUni(v.p);    /* VT_LPWSTR */
                PropVariantClear(ref v);
            }
            Marshal.ReleaseComObject(ps);
            return s;
        }

        static List<IMMDevice> Active(IMMDeviceEnumerator en, int flow) {
            List<IMMDevice> r = new List<IMMDevice>();
            IMMDeviceCollection col;
            uint n;
            if (en.EnumAudioEndpoints(flow, 1 /* DEVICE_STATE_ACTIVE */, out col) != 0 || col == null) return r;
            if (col.GetCount(out n) == 0) {
                for (uint i = 0; i < n; i++) {
                    IMMDevice d;
                    if (col.Item(i, out d) == 0 && d != null) r.Add(d);
                }
            }
            Marshal.ReleaseComObject(col);
            return r;
        }

        public static Endpoint[] List() {
            IMMDeviceEnumerator en = Enumerator();
            List<Endpoint> r = new List<Endpoint>();
            for (int flow = 0; flow < 2; flow++) {
                foreach (IMMDevice d in Active(en, flow)) {
                    Endpoint e = new Endpoint();
                    e.Flow = flow;
                    d.GetId(out e.Id);
                    e.Name = Name(d);
                    r.Add(e);
                    Marshal.ReleaseComObject(d);
                }
            }
            Marshal.ReleaseComObject(en);
            return r.ToArray();
        }

        /* Endpoint ID of the default for this flow and role (0 console, 1 multimedia, 2 communications), or null. */
        public static string DefaultId(int flow, int role) {
            IMMDeviceEnumerator en = Enumerator();
            IMMDevice d;
            string id = null;
            if (en.GetDefaultAudioEndpoint(flow, role, out d) == 0 && d != null) {
                d.GetId(out id);
                Marshal.ReleaseComObject(d);
            }
            Marshal.ReleaseComObject(en);
            return id;
        }

        public static int SetDefault(string id, int role) {
            IPolicyConfig pc = (IPolicyConfig)new PolicyConfigCo();
            int hr = pc.SetDefaultEndpoint(id, role);
            Marshal.ReleaseComObject(pc);
            return hr;
        }

        /* Active sessions (someone is playing or recording) on endpoints whose name contains filter. */
        public static Session[] ActiveSessions(string filter) {
            IMMDeviceEnumerator en = Enumerator();
            List<Session> r = new List<Session>();
            Guid iid = new Guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F");
            for (int flow = 0; flow < 2; flow++) {
                foreach (IMMDevice d in Active(en, flow)) {
                    string name = Name(d);
                    if (name.IndexOf(filter, StringComparison.OrdinalIgnoreCase) >= 0) {
                        object o;
                        if (d.Activate(ref iid, 23 /* CLSCTX_ALL */, IntPtr.Zero, out o) == 0 && o != null) {
                            IAudioSessionManager2 m = (IAudioSessionManager2)o;
                            IAudioSessionEnumerator se;
                            int n;
                            if (m.GetSessionEnumerator(out se) == 0 && se != null) {
                                if (se.GetCount(out n) == 0) {
                                    for (int i = 0; i < n; i++) {
                                        object so;
                                        if (se.GetSession(i, out so) != 0 || so == null) continue;
                                        IAudioSessionControl2 c = so as IAudioSessionControl2;
                                        int state;
                                        if (c != null && c.GetState(out state) == 0 && state == 1 /* Active */) {
                                            Session s = new Session();
                                            uint pid;
                                            s.Flow = flow;
                                            s.Endpoint = name;
                                            c.GetProcessId(out pid);
                                            s.ProcessId = pid;
                                            s.SystemSounds = c.IsSystemSoundsSession() == 0;
                                            r.Add(s);
                                        }
                                        Marshal.ReleaseComObject(so);
                                    }
                                }
                                Marshal.ReleaseComObject(se);
                            }
                            Marshal.ReleaseComObject(o);
                        }
                    }
                    Marshal.ReleaseComObject(d);
                }
            }
            Marshal.ReleaseComObject(en);
            return r.ToArray();
        }
    }
}
'@
}

$script:RoleNames = @('Console', 'Multimedia', 'Communications')
$script:FlowNames = @('playback', 'recording')

function Get-AudioEndpoints { [AviumAudio.Audio]::List() }

function Get-AudioDefaults {
    $names = @{}
    foreach ($e in [AviumAudio.Audio]::List()) { $names[$e.Id] = $e.Name }
    foreach ($flow in 0, 1) {
        foreach ($role in 0, 1, 2) {
            $id = [AviumAudio.Audio]::DefaultId($flow, $role)
            [pscustomobject]@{ Flow = $flow; Role = $role; Id = $id; Name = $(if ($id) { $names[$id] } else { '' }) }
        }
    }
}

function Format-AudioDefaults($defaults) {
    foreach ($d in $defaults) {
        '  {0,-9} {1,-14} {2}' -f $script:FlowNames[$d.Flow], $script:RoleNames[$d.Role], $(if ($d.Name) { $d.Name } else { '(none)' })
    }
}

# Put back every saved default that Windows moved, if that endpoint still exists. Returns the lines it printed.
function Restore-AudioDefaults($saved) {
    $active = @{}
    foreach ($e in [AviumAudio.Audio]::List()) { $active[$e.Id] = $e.Name }
    foreach ($d in $saved) {
        if (-not $d.Id) { continue }
        $now = [AviumAudio.Audio]::DefaultId($d.Flow, $d.Role)
        if ($now -eq $d.Id) { continue }
        $nowName = $(if ($now -and $active.ContainsKey($now)) { $active[$now] } else { '(none)' })
        if (-not $active.ContainsKey($d.Id)) {
            "  {0} {1}: was '{2}', which is gone; now '{3}'. Pick one in Sound settings if needed." -f $script:FlowNames[$d.Flow], $script:RoleNames[$d.Role], $d.Name, $nowName
            continue
        }
        $hr = [AviumAudio.Audio]::SetDefault($d.Id, $d.Role)
        if ($hr -eq 0) {
            "  {0} {1}: Windows had moved it to '{2}'; put back '{3}'." -f $script:FlowNames[$d.Flow], $script:RoleNames[$d.Role], $nowName, $d.Name
        } else {
            "  {0} {1}: could not put back '{2}' (0x{3:X8}). Fix by hand: mmsys.cpl -> right-click it -> Set as Default Device." -f $script:FlowNames[$d.Flow], $script:RoleNames[$d.Role], $d.Name, $hr
        }
    }
}

# -Filter: endpoint name text of one unit. revert-unit.ps1 passes '(Apollo Twin X'.
# [G2] '(Apollo x8p' without the closing parenthesis: Windows named the endpoints
# "MON L/R (Apollo x8p (open driver, test build))", which '(Apollo x8p)' never matched.
# [Endpoints] Default '(Apollo': every Apollo endpoint, "(Apollo x8p)", "(Apollo Twin X)" and the combined
# set's "(Apollo)" (the x8p hosts it, so an app on it holds the x8p too).
function Get-ApolloAudioSessions([string]$Filter = '(Apollo') {
    foreach ($s in [AviumAudio.Audio]::ActiveSessions($Filter)) {
        $p = Get-Process -Id $s.ProcessId -ErrorAction SilentlyContinue
        [pscustomobject]@{
            Flow     = $script:FlowNames[$s.Flow]
            Endpoint = $s.Endpoint
            Pid      = $s.ProcessId
            Process  = $(if ($s.SystemSounds) { 'Windows system sounds' } elseif ($p) { $p.ProcessName } else { '?' })
        }
    }
}

# Everything that holds the x8p. Returns a list of lines; empty = idle. $diag: path to apollo-diag.exe.
function Test-ApolloInUse([string]$diag) {
    $why = @()
    foreach ($n in 'asio-test', 'loopback-test', 'wasapi-loop', 'apollo-diag', 'Ableton Live*') {
        foreach ($p in @(Get-Process -Name $n -ErrorAction SilentlyContinue)) {
            $why += "process $($p.ProcessName) (PID $($p.Id)) has the Apollo open"
        }
    }
    if ($diag -and (Test-Path $diag)) {
        # [G2] the x8p's handle explicitly: with a Twin X on apollo.sys too, the counters must be the x8p's
        $ep = (& $diag --unit x8p endpoints 2>&1 | Out-String)
        if ($ep -match 'ACX streams: prepared (\d+), running (\d+)') {
            if ([int]$Matches[2] -gt 0) { $why += "$($Matches[2]) Windows audio stream(s) RUNNING on the x8p (Windows refuses to remove the driver under them)" }
            elseif ([int]$Matches[1] -gt 0) { $why += "$($Matches[1]) Windows audio stream(s) open (paused) on the x8p" }
        }
        $en = (& $diag --unit x8p engine 2>&1 | Out-String)
        if ($en -match '(?m)^ASIO\s+(running|open)') { $why += "an ASIO host has the Apollo x8p (Avium) driver $($Matches[1])" }
    }
    try {
        foreach ($s in @(Get-ApolloAudioSessions)) {
            $why += "$($s.Process) (PID $($s.Pid)) is using $($s.Endpoint) for $($s.Flow)"
        }
    } catch {
        $why += "(could not list audio sessions: $($_.Exception.Message))"
    }
    return ,$why
}

# Ask the user to free the x8p before PnP touches the driver. Returns $true to go ahead, $false to stop.
function Wait-ApolloIdle([string]$diag, [switch]$Yes, [scriptblock]$Note = { param($s) Write-Host $s }) {
    $why = Test-ApolloInUse $diag
    if ($why.Count -eq 0) { return $true }
    & $Note 'The x8p is in use:'
    foreach ($w in $why) { & $Note "  - $w" }
    & $Note 'Close those programs, or stop their playback/recording (Ableton: Preferences -> Audio -> Driver Type away from ASIO).'
    & $Note 'Waiting up to 60 s for the x8p to be free...'
    $deadline = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 2
        $why = Test-ApolloInUse $diag
        if ($why.Count -eq 0) { & $Note 'The x8p is free.'; return $true }
    }
    & $Note 'Still in use after 60 s:'
    foreach ($w in $why) { & $Note "  - $w" }
    if ($Yes) {
        & $Note '-Yes given: going ahead. If Windows cannot stop the driver it asks for a reboot; nothing is lost.'
        return $true
    }
    while ($true) {
        $a = Read-Host '[Enter] go ahead anyway (Windows may ask for a reboot), [R] restart the Windows audio service (all sound stops for a few seconds), [Q] quit'
        if ($a -eq '' -or $a -match '^[Gg]') { return $true }
        if ($a -match '^[Qq]') { return $false }
        if ($a -match '^[Rr]') {
            & $Note 'Restarting the Windows audio service (audiosrv)...'
            Restart-Service -Name audiosrv -Force -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 3
            $why = Test-ApolloInUse $diag
            if ($why.Count -eq 0) { & $Note 'The x8p is free.'; return $true }
            foreach ($w in $why) { & $Note "  - $w" }
        }
    }
}
