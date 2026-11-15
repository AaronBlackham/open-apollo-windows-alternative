# SPDX-License-Identifier: GPL-2.0-only
"""Windows equivalents of the mapper's Linux-only calls (added by Avium for the Windows port,
2026-11-14). On Linux every function does exactly what the original code did; on Windows:

- fcntl.flock(f, LOCK_EX | LOCK_NB) / LOCK_EX  ->  msvcrt.locking on byte 0 of the lock file
  (non-blocking, or retried until granted). Like flock, a second handle on the same file (in this
  or another process) is refused; the lock goes when it is released or the handle is closed.
- Popen(start_new_session=True) + os.killpg(pid, SIGKILL)  ->  CREATE_NEW_PROCESS_GROUP and a Job
  Object holding the child; TerminateJobObject kills it and everything it started. (A venv's
  python.exe is a launcher that runs the real interpreter as its child: the job holds both.)
- multiprocessing 'fork'  ->  'spawn' (Windows has no fork).
- Linux default folders (/lib/firmware, XDG_*, ~/.local/share)  ->  %LOCALAPPDATA%\\Avium\\ApolloPlugins.
- UTF-8: Linux reads and writes text as UTF-8; Windows defaults to the ANSI code page, which would
  silently misread the panel specs. require_utf8() insists on Python's UTF-8 mode and passes it to
  every child process (PYTHONUTF8=1).
- os.replace onto a file another process has open fails on Windows; replace() retries briefly.
"""
import os
import sys
import time
from pathlib import Path

WINDOWS = os.name == 'nt'

if WINDOWS:
    import ctypes
    import msvcrt
    from ctypes import wintypes
else:
    import fcntl
    import signal


# -- folders ----------------------------------------------------------------------------------

def data_root():
    """%LOCALAPPDATA%\\Avium\\ApolloPlugins (docs/PLUGINS.md 2.6)."""
    return Path(os.environ.get('LOCALAPPDATA') or Path.home() / 'AppData/Local') / 'Avium' / 'ApolloPlugins'


# -- UTF-8 ------------------------------------------------------------------------------------

def require_utf8():
    if WINDOWS:
        if not sys.flags.utf8_mode:
            sys.exit('plugin_mapper on Windows needs Python\'s UTF-8 mode (as Linux reads text): '
                     'run it with `python -X utf8 ...` or set PYTHONUTF8=1')
        os.environ['PYTHONUTF8'] = '1'   # every child process too


# -- locks ------------------------------------------------------------------------------------

def _region(f):
    fd = f.fileno()
    os.lseek(fd, 0, os.SEEK_SET)   # msvcrt.locking locks from the current position
    return fd


def lock_nb(f):
    """fcntl.flock(f, LOCK_EX | LOCK_NB): raises OSError when another handle holds it."""
    if not WINDOWS:
        return fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    try:
        msvcrt.locking(_region(f), msvcrt.LK_NBLCK, 1)
    except OSError as error:
        raise BlockingIOError(error.errno, 'lock held by another handle') from error


def lock(f):
    """fcntl.flock(f, LOCK_EX): waits until granted."""
    if not WINDOWS:
        return fcntl.flock(f, fcntl.LOCK_EX)
    while True:
        try:
            return msvcrt.locking(_region(f), msvcrt.LK_NBLCK, 1)
        except OSError:
            time.sleep(0.05)


def unlock(f):
    if not WINDOWS:
        return fcntl.flock(f, fcntl.LOCK_UN)
    msvcrt.locking(_region(f), msvcrt.LK_UNLCK, 1)


class held:
    """`with open(...) as f, held(f):` = flock(f, LOCK_EX) for the block, released before f closes."""

    def __init__(self, f):
        self.f = f

    def __enter__(self):
        lock(self.f)
        return self.f

    def __exit__(self, *exc):
        unlock(self.f)
        return False


# -- process groups ---------------------------------------------------------------------------

if WINDOWS:
    _k32 = ctypes.WinDLL('kernel32', use_last_error=True)   # Windows' own kernel32, for job objects
    _k32.CreateJobObjectW.restype = wintypes.HANDLE
    _k32.CreateJobObjectW.argtypes = (wintypes.LPVOID, wintypes.LPCWSTR)
    _k32.AssignProcessToJobObject.argtypes = (wintypes.HANDLE, wintypes.HANDLE)
    _k32.TerminateJobObject.argtypes = (wintypes.HANDLE, wintypes.UINT)
    _k32.CloseHandle.argtypes = (wintypes.HANDLE,)


def new_group():
    """Popen keyword arguments for a child that starts its own process group."""
    if not WINDOWS:
        return dict(start_new_session=True)
    import subprocess
    return dict(creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)


class Group:
    """The process group of a child started with new_group(); kill() = os.killpg(pid, SIGKILL)."""

    def __init__(self, proc):
        self.proc, self.job = proc, None
        if WINDOWS:
            job = _k32.CreateJobObjectW(None, None)
            if job and _k32.AssignProcessToJobObject(job, int(proc._handle)):
                self.job = job
            elif job:
                _k32.CloseHandle(job)

    def kill(self):
        if not WINDOWS:
            return os.killpg(self.proc.pid, signal.SIGKILL)
        if self.job is not None:
            _k32.TerminateJobObject(self.job, 1)
        try:
            self.proc.kill()   # also when the job could not be made (the launcher's own job ends its child)
        except OSError:
            pass

    def close(self):
        if WINDOWS and self.job is not None:
            _k32.CloseHandle(self.job)
            self.job = None


# -- multiprocessing --------------------------------------------------------------------------

def process_context():
    import multiprocessing
    return multiprocessing.get_context('spawn' if WINDOWS else 'fork')


# -- atomic replace ---------------------------------------------------------------------------

def replace(tmp, path, attempts=50):
    """tmp.replace(path); on Windows retried while a reader holds `path` open (up to ~2.5 s)."""
    for n in range(attempts):
        try:
            return Path(tmp).replace(path)
        except PermissionError:
            if not WINDOWS or n == attempts - 1:
                raise
            time.sleep(0.05)
