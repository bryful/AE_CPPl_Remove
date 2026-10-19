import os
import ctypes
import pathlib
import struct
import subprocess
import tempfile
import unittest

EXE = pathlib.Path(__file__).resolve().parents[1] / 'x64/Release/delCppl.exe'
def chunk(tag, data):
    return tag + struct.pack('>I', len(data)) + data + (b'\x00' if len(data) & 1 else b'')
def project(*chunks):
    return chunk(b'RIFX', b'Egg!' + b''.join(chunks))
def cppl(data=None):
    return chunk(b'LIST', b'CPPl' + (chunk(b'pprf', b'preferences CPPl cpid LIST') if data is None else data))

class CliTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='delCppl_', dir=pathlib.Path(__file__).resolve().parent)
        self.root = pathlib.Path(self.temp.name)
    def tearDown(self):
        assert self.root.resolve().parent == pathlib.Path(__file__).resolve().parent
        self.temp.cleanup()
    def run_cli(self, *args, code=0):
        result = subprocess.run([str(EXE), *map(str, args)], capture_output=True, encoding='utf-8')
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        return result.stdout + result.stderr
    def make(self, name='日本語.aep', data=None):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data if data is not None else project(chunk(b'head', b'abc'), cppl(), chunk(b'cpid', b'1234')))
        return path
    def test_info_and_patch_preserve_bytes(self):
        path = self.make()
        original = path.read_bytes()
        self.assertIn('CPPl内容:', self.run_cli(path))
        self.assertEqual(list(self.root.iterdir()), [path])
        self.run_cli('-patch', path, '-info')
        expected = project(chunk(b'head', b'abc'), cppl(b''), chunk(b'cpid', b'1234'))
        self.assertEqual(path.read_bytes(), original)
        self.assertEqual((self.root / '日本語_patched.aep').read_bytes(), expected)
        self.run_cli(path, '-f')
        self.assertEqual((self.root / '日本語_patched_1.aep').read_bytes(), expected)
    def test_overwrite_and_idempotence(self):
        path = self.make()
        original = path.read_bytes()
        self.run_cli('-p', '-overwrite', path)
        backups = list(self.root.glob('*_original_*.aep'))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), original)
        self.assertEqual(path.read_bytes(), project(chunk(b'head', b'abc'), cppl(b''), chunk(b'cpid', b'1234')))
        self.run_cli('-p', '-overwrite', path)
        self.assertEqual(len(list(self.root.iterdir())), 2)
    def test_search_sub_wildcards_and_deduplication(self):
        a = self.make('a1.aep')
        self.make('a2.AEP')
        self.make('sub/a3.aep')
        self.make('excluded_patched.aep')
        self.make('excluded_original_20261019_123456_1.aep')
        self.make('other.aep')
        text = self.run_cli(self.root / 'a?.aep', a, '-p', '-sub')
        self.assertIn('削除: 3', text)
        self.assertEqual(len(list(self.root.rglob('*_patched.aep'))), 4)
        text = self.run_cli(self.root, '-sub')
        self.assertIn('確認: 4', text)
        self.assertIn('確認: 1', self.run_cli(self.root / 'excluded_patched.aep'))
    def test_no_cppl_and_empty(self):
        self.make('none.aep', project(chunk(b'text', b'CPPl LIST cpid')))
        self.make('empty.aep', project(cppl(b'')))
        self.assertIn('変更不要: 2', self.run_cli(self.root, '-p'))
        self.assertEqual(len(list(self.root.iterdir())), 2)
    def test_invalid_multiple_nested_and_continue(self):
        self.make('truncated.aep', project(cppl())[:-1])
        self.make('multiple.aep', project(cppl(), cppl()))
        self.make('nested.aep', project(chunk(b'LIST', b'test' + cppl())))
        self.make('overflow.aep', project(b'LIST\xff\xff\xff\xffCPPl'))
        self.make('small-list.aep', project(chunk(b'LIST', b'abc')))
        self.make('wrong-form.aep', b'RIFF\x00\x00\x00\x04Egg!')
        self.make('good.aep')
        self.assertIn('削除: 1', self.run_cli(self.root, '-p', code=1))
        self.assertEqual(len(list(self.root.glob('*_patched.aep'))), 1)
        self.assertEqual(len(list(self.root.glob('*.tmp'))), 0)
    def test_arguments(self):
        self.run_cli('--help')
        self.run_cli('-unknown', code=2)
        self.run_cli('-p', code=2)
        self.run_cli(self.root, '-overwrite', code=2)
        self.run_cli(self.root / 'missing.aep', code=1)
        self.run_cli(self.root / '*.aep', code=1)
        self.run_cli(self.root / '*' / '*.aep', code=1)
    def test_readonly_input_and_output_conflict(self):
        path = self.make()
        existing = self.make('日本語_patched.aep', b'keep')
        os.chmod(path, 0o444)
        try:
            self.run_cli(path, '-p')
            self.assertEqual(existing.read_bytes(), b'keep')
            self.assertTrue((self.root / '日本語_patched_1.aep').exists())
        finally:
            os.chmod(path, 0o666)

    def test_failed_rename_preserves_original(self):
        path = self.make()
        original = path.read_bytes()
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p,
                                      ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = kernel.CreateFileW(str(path), 0x80000000, 1, None, 3, 0, None)
        self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
        try:
            self.run_cli(path, '-p', '-overwrite', code=1)
            self.assertEqual(path.read_bytes(), original)
            self.assertEqual(list(self.root.iterdir()), [path])
        finally:
            kernel.CloseHandle(handle)

    def test_long_and_extended_path(self):
        relative = '/'.join(['long_directory_' + 'x' * 40] * 5) + '/project.aep'
        path = self.make(relative)
        self.run_cli('\\\\?\\' + str(path), '-p')
        self.assertTrue(path.with_stem('project_patched').exists())

if __name__ == '__main__':
    unittest.main(verbosity=2)

