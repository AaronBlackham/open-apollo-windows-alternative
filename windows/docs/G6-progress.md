# G6 progress (combined device), Nov 14-15 2026

Work copy: `builds\work-g6` (base `builds\base-pre-g56`). Resume from here if the PC rebooted.

## Done
- Drift measured (`tools\agg-test\agg-drift.exe 60`, both units on build g4b): **Twin X runs 4.74 ppm slower
  than the x8p** (x8p 47,999.949 frames/s, Twin X 47,999.722 against QPC; 13.7 frames apart after 60 s;
  read jitter 0.3 frame rms). CSV of the run: replayed by `asrc-test --trace`.
- Peer-clock survey (read-only): 0x2280-0x229C, 0x22C4, 0x22C8-0x22D8, 0xC5B8-0xC5C0 all 0 on both units;
  0x224C x8p 0x121, Twin X 0x102. Topology: Twin X behind the x8p's Alpine Ridge switch (downstream port 4),
  ACS capability register 0 on the switch ports; DMA remapping on (our INF), Verifier DMA checks on.
- Code: `driver\agg.c`, `driver\apollo_agg.h`, `include\apollo_agg.h`, hooks in engine.c, device.c, driver.c,
  ioctl.c, asio.c; G4 review fixes in stream.c (provisional slots re-resolved at engine start) and device.c
  (circuits removed after a failed Twin X bring-up). `tools\agg-test\` (asrc-test offline: 105/105 incl. the
  real trace; agg-test, agg-drift).

## Install 1 (driver only, both units)
- Package `driver\x64\Release\apollo\`; pnputil /add-driver, devcon update both hwids, warm restarts.
- Expect: both units Connected Mode 2 as before; `agg-test state` shows the default config (x8p first, Twin X
  master), the x8p follower LOCKED at about +4.7 ppm.
- Rollback: reinstall `builds\2026-11-14-g4b\package\apollo.inf` the same way.

## Next
- Peer-clock probe (`agg-test probe --unit x8p --source 0x0A`, then back to 0x0C).
- ASIO DLL "Apollo (Avium)" (second class in avium-asio.dll), asio-test --agg, Apollo Control agg-get/agg-set.

## Results so far (install 1, Nov 15 ~00:30)
- Install 1 OK: both units Connected Mode 2; default config, Twin X timeline, x8p follower LOCKED +4.76 ppm,
  e within +/-0.09 frame, aggregate work 1-2 us idle.
- Peer clock: source 0x0A acked by both units (10-42 ms) but 0xC07C drops to 0x200 and the unit free-runs
  (drift unchanged -4.78/-4.74 ppm vs -4.79 baseline). Both restored to 0x20C.
- asio-test --agg 20 128 (routes none): PASS, 7500 switches, 0 underruns/overruns/resyncs, both MON
  loopbacks -20.00 dBFS, residual 120.7 / 115.5 dB below the tone, phase constant to 0.000 deg over 19 s.
- Next: Twin X loss during playback (disable/enable while asio-test runs).
- Twin X loss during playback: no bugcheck, x8p acting master, stream ran on (0 underruns), host got
  kAsioResetRequest, Twin X re-joined as master, x8p re-locked.
- 64-frame buffers: about 3 skipped switches/s in asio-test with BOTH the combined and the unchanged
  x8p-only class (host/PC, not G6). 128 frames: clean on both. Aggregate work max 51 us at 64 frames.
- Apollo Control host: agg-get/agg-set implemented (app\src\agg_json.h + main.cpp); app tests pass.
- build-all green (agg-test added: asrc-test 101, aggjson-test 22).

## Install 2 (driver with x8p MIC/LINE 1-8 names; ASIO DLL to C:\Program Files\Avium\ASIO + regsvr32)
- Stored config right now: routes none (test). Final: `agg-test set --routes studio`.
- Rollback ASIO: copy builds\2026-11-14-g4b\bin\avium-asio.dll back, regsvr32 /u the new one first.
- Install 2 OK: 30 s asio-test --agg PASS (11,250 switches, 0/0/0, phase 0.000 deg), x8p MIC/LINE 1-8 names;
  avium-asio.dll copied to C:\Program Files\Avium\ASIO (old one kept as avium-asio.dll.g4b-backup) and
  registered: "Apollo (Avium)" + "Apollo x8p (Avium)". Stored config: Studio.

## Install 3 (final: routed outputs named "x8p MON L > TwinX"; spec 6.9.5 written)
- Same procedure; then routes none -> asio-test --agg 15 -> routes studio (final stored config).
- Install 3 (oem166, saved as builds\2026-11-14-g6\package-installed): 15 s asio-test --agg PASS (5,624
  switches, 0/0/0, phase 0.000 deg); routed outputs named "x8p MON L > TwinX"; Studio stored (final).
- **x8p state after install 3:** its disable was vetoed (an app had its endpoints open), Windows queued it for
  the next reboot (ConfigFlags 1). `devcon enable @<x8p id>` cleared the flag (ConfigFlags 0: it comes up
  enabled after a reboot) but IsRebootRequired stays True: **the x8p cannot be restarted before a reboot**,
  so no further driver can be loaded tonight. It runs fine on oem166.
- After install 3: in-place routing (AggAsioResolveOutputs) built, CA clean, NOT installed:
  builds\2026-11-14-g6\package-next. Install it after the next reboot and test: agg-test set --routes none /
  studio while asio-test --agg runs (x8p MON pair must move without a reset request).
