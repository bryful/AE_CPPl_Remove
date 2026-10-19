"""Native Windows UI integration test. Launches its app window hidden."""
import ctypes as c
from ctypes import wintypes as w
import pathlib
import subprocess
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
u = c.WinDLL('user32', use_last_error=True)
u.FindWindowW.argtypes = [w.LPCWSTR, w.LPCWSTR]; u.FindWindowW.restype = w.HWND
u.GetDlgItem.argtypes = [w.HWND, c.c_int]; u.GetDlgItem.restype = w.HWND
u.SendMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]; u.SendMessageW.restype = w.LPARAM
u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
u.GetDlgCtrlID.argtypes = [w.HWND]; u.GetDlgCtrlID.restype = c.c_int
u.ShowWindow.argtypes = [w.HWND, c.c_int]
u.IsWindowEnabled.argtypes = [w.HWND]; u.IsWindowEnabled.restype = w.BOOL
CALLBACK = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
u.EnumChildWindows.argtypes = [w.HWND, CALLBACK, w.LPARAM]

def wait_for(predicate, timeout=8):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = predicate()
        if value:
            return value
        time.sleep(.02)
    raise AssertionError('UI timeout')

def text(hwnd):
    buffer = c.create_unicode_buffer(32768)
    u.SendMessageW(hwnd, 0xD, len(buffer), c.addressof(buffer))
    return buffer.value

def set_text(hwnd, value):
    buffer = c.create_unicode_buffer(value)
    u.SendMessageW(hwnd, 0xC, 0, c.addressof(buffer))

def click(hwnd, id):
    u.PostMessageW(hwnd, 0x111, id, 0)

def find_filename_edit(dialog):
    found = []
    def child(hwnd, _):
        cls = c.create_unicode_buffer(100)
        u.GetClassNameW(hwnd, cls, len(cls))
        if cls.value == 'Edit' and u.GetDlgCtrlID(hwnd) in (1001, 1148):
            found.append(hwnd)
        return True
    u.EnumChildWindows(dialog, CALLBACK(child), 0)
    return found[0] if found else None

def choose(hwnd, button, title, path=None):
    click(hwnd, button)
    dialog = wait_for(lambda: u.FindWindowW('#32770', title))
    u.ShowWindow(dialog, 0)
    if path is None:
        click(dialog, 2)
    else:
        edit = wait_for(lambda: find_filename_edit(dialog))
        time.sleep(.5)
        set_text(edit, str(path))
        click(dialog, 1)
    wait_for(lambda: not u.FindWindowW('#32770', title))

from test_cli import project, chunk, cppl
def run():
    with tempfile.TemporaryDirectory(prefix='ui_', dir=ROOT / 'tests') as folder:
        folder = pathlib.Path(folder)
        assert folder.resolve().parent == ROOT / 'tests'
        source = folder / '日本語.aep'
        empty = folder / 'empty.aep'
        bad = folder / 'bad.aep'
        original = project(chunk(b'head', b'abc'), cppl(), chunk(b'cpid', b'1234'))
        source.write_bytes(original)
        empty.write_bytes(project(cppl(b'')))
        bad.write_bytes(b'bad')
        startup = subprocess.STARTUPINFO()
        startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        process = subprocess.Popen([str(ROOT / 'x64/Release/delCpplUI.exe'), str(source), str(empty)], startupinfo=startup)
        hwnd = None
        try:
            hwnd = wait_for(lambda: u.FindWindowW('delCpplUIWindow', 'delCpplUI'))
            status = lambda: text(u.GetDlgItem(hwnd, 1012))
            wait_for(lambda: status() == '解析完了')
            assert 'CPPl内容: 34 bytes' in text(u.GetDlgItem(hwnd, 1011))
            assert text(u.GetDlgItem(hwnd, 1010)) == str(source)
            assert u.IsWindowEnabled(u.GetDlgItem(hwnd, 1002))
            assert u.SendMessageW(hwnd, 0x7F, 0, 0), 'small app icon is missing'
            assert u.SendMessageW(hwnd, 0x7F, 1, 0), 'large app icon is missing'
            print('startup first file / CPPl information: PASS')
            for label in ('テーマ: ダーク', 'テーマ: ライト', 'テーマ: 自動'):
                click(hwnd, 1004)
                wait_for(lambda: text(u.GetDlgItem(hwnd, 1004)) == label)
                assert 'CPPl内容: 34 bytes' in text(u.GetDlgItem(hwnd, 1011))
            print('dark / light / automatic theme switching preserves file information: PASS')
            choose(hwnd, 1001, 'AEPファイルを選択')
            assert text(u.GetDlgItem(hwnd, 1010)) == str(source)
            choose(hwnd, 1002, 'CPPl削除後のAEPを保存')
            assert not (folder / '日本語_patched.aep').exists()
            print('open and save dialog cancellation: PASS')
            saved = folder / '保存先.aep'
            choose(hwnd, 1002, 'CPPl削除後のAEPを保存', saved)
            wait_for(lambda: status() == '保存完了')
            assert saved.read_bytes() == project(chunk(b'head', b'abc'), cppl(b''), chunk(b'cpid', b'1234'))
            assert source.read_bytes() == original
            print('save dialog / patched byte equality / original preservation: PASS')
            choose(hwnd, 1001, 'AEPファイルを選択', empty)
            wait_for(lambda: status() == '解析完了')
            assert 'CPPlは空です' in text(u.GetDlgItem(hwnd, 1011))
            assert not u.IsWindowEnabled(u.GetDlgItem(hwnd, 1002))
            choose(hwnd, 1001, 'AEPファイルを選択', bad)
            wait_for(lambda: status() == '失敗しました')
            assert '解析失敗' in text(u.GetDlgItem(hwnd, 1011))
            assert not u.IsWindowEnabled(u.GetDlgItem(hwnd, 1002))
            print('file selection / empty and corrupt files disable execution: PASS')
        finally:
            if hwnd:
                u.PostMessageW(hwnd, 0x10, 0, 0)
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait()
        assert process.returncode == 0
    
    
    
    

if __name__ == '__main__':
    run()
