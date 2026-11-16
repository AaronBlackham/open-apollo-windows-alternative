# SPDX-License-Identifier: GPL-2.0-only
"""Self-test of plugin_mapper/winshim.py on this PC: locks and process-group kill (no UA code involved).

  python -X utf8 winshim_selftest.py
"""
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / 'open-apollo/tools/plugin-mapper/plugin_mapper'))
import winshim  # noqa: E402

ok = True
with tempfile.TemporaryDirectory() as tmp:
    lock = Path(tmp) / '.lock'
    a, b = open(lock, 'a'), open(lock, 'a')
    winshim.lock_nb(a)
    try:
        winshim.lock_nb(b)
        print('FAIL  a second handle got the lock'); ok = False
    except OSError:
        print('PASS  lock_nb refuses a second handle (like flock LOCK_NB)')
    # another process is refused too
    child = subprocess.run([sys.executable, '-c', f'import sys;sys.path.insert(0,{str(Path(winshim.__file__).parent)!r});'
                            f'import winshim;f=open({str(lock)!r},"a")\ntry:\n winshim.lock_nb(f);print("got")\nexcept OSError:print("refused")'],
                           capture_output=True, text=True)
    print(('PASS' if child.stdout.strip() == 'refused' else 'FAIL') + f'  another process: {child.stdout.strip()}')
    ok &= child.stdout.strip() == 'refused'
    winshim.unlock(a)
    with winshim.held(b):
        print('PASS  held() takes it after unlock')
    a.close(); b.close()

    # process group: a worker that starts a grandchild; kill() ends both (os.killpg semantics)
    marker = Path(tmp) / 'grandchild.pid'
    code = (f'import subprocess,sys,time;p=subprocess.Popen([sys.executable,"-c","import os,time;'
            f'open({marker.as_posix()!r},\\"w\\").write(str(os.getpid()));time.sleep(120)"]);time.sleep(120)')
    proc = subprocess.Popen([sys.executable, '-c', code], **winshim.new_group())
    group = winshim.Group(proc)
    job = group.job is not None                     # read now: close() below sets it to None
    for _ in range(100):
        if marker.exists() and marker.read_text():
            break
        time.sleep(0.1)
    gpid = int(marker.read_text())
    group.kill(); proc.wait(); group.close(); time.sleep(0.5)
    alive = str(gpid) in subprocess.run(['tasklist', '/FI', f'PID eq {gpid}'], capture_output=True, text=True).stdout
    print(('FAIL' if alive else 'PASS') + f'  Group.kill() ended the worker and its grandchild (job: {job})')
    ok &= not alive
print('all pass' if ok else 'FAILURES')
sys.exit(0 if ok else 1)
