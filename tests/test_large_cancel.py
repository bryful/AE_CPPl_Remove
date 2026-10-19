import ctypes
import msvcrt
import pathlib
import struct
import subprocess
import tempfile
import time

root = pathlib.Path(__file__).resolve().parent
exe = root.parent / 'x64/Release/delCppl.exe'
kernel = ctypes.WinDLL('kernel32', use_last_error=True)
kernel.DeviceIoControl.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong,
                                  ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.c_void_p]
with tempfile.TemporaryDirectory(prefix='large_', dir=root) as folder:
    folder = pathlib.Path(folder)
    assert folder.resolve().parent == root
    path = folder / 'large.aep'
    n = 3 * 1024**3
    with path.open('wb') as stream:
        returned = ctypes.c_ulong()
        if not kernel.DeviceIoControl(msvcrt.get_osfhandle(stream.fileno()), 0x900C4, None, 0, None, 0, ctypes.byref(returned), None):
            raise ctypes.WinError()
        stream.write(b'RIFX' + struct.pack('>I', 24+n) + b'Egg!LIST' + struct.pack('>I', 12+n) + b'CPPlpprf' + struct.pack('>I', n))
        stream.seek(n-1, 1)
        stream.write(b'\0')
    result = subprocess.run([str(exe), str(path), '-p'], capture_output=True)
    assert result.returncode == 0, result.stdout + result.stderr
    expected = b'RIFX\0\0\0\x10Egg!LIST\0\0\0\x04CPPl'
    assert (folder / 'large_patched.aep').read_bytes() == expected
    print('3 GiB CPPl: PASS')
    path = folder / 'cancel.aep'
    with path.open('wb') as stream:
        returned = ctypes.c_ulong()
        if not kernel.DeviceIoControl(msvcrt.get_osfhandle(stream.fileno()), 0x900C4, None, 0, None, 0, ctypes.byref(returned), None):
            raise ctypes.WinError()
        cppl = b'LIST\0\0\0\x10CPPlpprf\0\0\0\x04test'
        stream.write(b'RIFX' + struct.pack('>I', 4+len(cppl)+8+n) + b'Egg!' + cppl + b'data' + struct.pack('>I', n))
        stream.seek(n-1, 1)
        stream.write(b'\0')
    original_size = path.stat().st_size
    p = subprocess.Popen([str(exe), str(path), '-p', '-overwrite'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    deadline = time.monotonic()+10
    while p.poll() is None and time.monotonic() < deadline:
        if list(folder.glob('.delCppl_*.tmp')):
            break
        time.sleep(.001)
    if not ctypes.windll.kernel32.GenerateConsoleCtrlEvent(1, p.pid):
        p.terminate(); p.communicate()
        raise ctypes.WinError()
    out, err = p.communicate(timeout=15)
    assert p.returncode == 130, (p.returncode, out, err)
    assert path.stat().st_size == original_size
    assert not list(folder.glob('.delCppl_*.tmp'))
    assert not list(folder.glob('cancel_original*'))
    print('Ctrl+Break during copy, cleanup and original preservation: PASS')
