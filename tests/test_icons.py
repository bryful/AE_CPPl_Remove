"""Validate every embedded icon frame against the source ICO and Windows decoder."""
import ctypes as c
from ctypes import wintypes as w
import pathlib
import struct
root=pathlib.Path(__file__).resolve().parents[1]
k=c.WinDLL('kernel32',use_last_error=True)
u=c.WinDLL('user32',use_last_error=True)
k.LoadLibraryExW.argtypes=[w.LPCWSTR,w.HANDLE,w.DWORD];k.LoadLibraryExW.restype=w.HMODULE
k.FreeLibrary.argtypes=[w.HMODULE]
k.FindResourceW.argtypes=[w.HMODULE,c.c_void_p,c.c_void_p];k.FindResourceW.restype=c.c_void_p
k.LoadResource.argtypes=[w.HMODULE,c.c_void_p];k.LoadResource.restype=c.c_void_p
k.SizeofResource.argtypes=[w.HMODULE,c.c_void_p];k.SizeofResource.restype=w.DWORD
k.LockResource.argtypes=[c.c_void_p];k.LockResource.restype=c.c_void_p
u.CreateIconFromResourceEx.argtypes=[c.c_void_p,w.DWORD,w.BOOL,w.DWORD,c.c_int,c.c_int,w.UINT]
u.CreateIconFromResourceEx.restype=w.HICON
u.DestroyIcon.argtypes=[w.HICON]
def resource(module,id,type):
    location=k.FindResourceW(module,id,type)
    assert location, c.get_last_error()
    loaded=k.LoadResource(module,location)
    return c.string_at(k.LockResource(loaded),k.SizeofResource(module,location))
for name in ('delCppl','delCpplUI'):
    ico=(root/'assets/icons'/(name+'.ico')).read_bytes()
    reserved,type,count=struct.unpack_from('<HHH',ico)
    assert (reserved,type,count)==(0,1,9)
    module=k.LoadLibraryExW(str(root/'x64/Release'/(name+'.exe')),None,0x22)
    assert module,c.get_last_error()
    try:
        group=resource(module,101,14)
        assert struct.unpack_from('<HHH',group)==(0,1,count)
        for i,size in enumerate((16,20,24,32,40,48,64,128,256)):
            width,height,colors,zero,planes,bits,length,offset=struct.unpack_from('<BBBBHHII',ico,6+16*i)
            entry=struct.unpack_from('<BBBBHHIH',group,6+14*i)
            assert (width or 256)==size and (height or 256)==size and bits==32
            assert entry[:7]==(width,height,colors,zero,planes,bits,length)
            data=resource(module,entry[-1],3)
            assert data==ico[offset:offset+length]
            raw=c.create_string_buffer(data)
            icon=u.CreateIconFromResourceEx(raw,len(data),True,0x30000,size,size,0)
            assert icon,(name,size,c.get_last_error())
            u.DestroyIcon(icon)
        print(name+': 9 embedded frames match ICO; Windows decoding: PASS')
    finally:
        k.FreeLibrary(module)
