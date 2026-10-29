"""Read files from the client's MPQ chain (highest priority first) and DBCs."""
import ctypes, os, struct, io
D = '/root/tswow-server/tswow-client/Data'
ORDER = ['patch-Y.MPQ','patch-V.MPQ','patch-T.MPQ','patch-R.MPQ','patch-H.MPQ','patch-G.MPQ','patch-F.MPQ','patch-E.MPQ',
         'patch-D.MPQ','patch-C.MPQ','patch-B.MPQ','patch-A.MPQ','patch-3.MPQ','patch-2.MPQ','patch.MPQ',
         'enUS/patch-enUS-3.MPQ','enUS/patch-enUS-2.MPQ','enUS/patch-enUS.MPQ','lichking.MPQ','expansion.MPQ',
         'common-2.MPQ','common.MPQ','enUS/locale-enUS.MPQ']
lib = ctypes.CDLL('libstorm.so.9')
lib.SFileGetFileSize.restype = ctypes.c_uint32
_h = []
for n in ORDER:
    p = os.path.join(D, n)
    if os.path.isfile(p):
        h = ctypes.c_void_p()
        if lib.SFileOpenArchive(p.encode(), 0, 0x100, ctypes.byref(h)):
            _h.append(h)

def read(name):
    for h in _h:
        hf = ctypes.c_void_p()
        if lib.SFileOpenFileEx(h, name.encode(), 0, ctypes.byref(hf)):
            size = lib.SFileGetFileSize(hf, None)
            buf = ctypes.create_string_buffer(size); got = ctypes.c_uint32()
            lib.SFileReadFile(hf, buf, size, ctypes.byref(got), None); lib.SFileCloseFile(hf)
            return buf.raw[:got.value]
    return None

def dbc(name):
    b = read('DBFilesClient\\' + name)
    magic, nrec, nf, rs, ss = struct.unpack('<4s4I', b[:20])
    recs = [struct.unpack('<%dI' % nf, b[20 + i * rs:20 + i * rs + nf * 4]) for i in range(nrec)]
    sb = b[20 + nrec * rs:]
    s = lambda o: sb[o:sb.index(b'\0', o)].decode('latin1')
    return {r[0]: r for r in recs}, s
