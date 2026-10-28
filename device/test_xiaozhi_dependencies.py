"""Host test dependency discovery; uses installed pkg-config and system loader."""
import ctypes.util
import functools
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

@functools.cache
def native_lib_flags():
    extra=["-pthread", "-lm"]
    if sys.platform.startswith("linux"):
        extra.append("-ldl")
    if shutil.which("pkg-config"):
        return shlex.split(subprocess.check_output(["pkg-config", "--libs", "json-c", "opus"], text=True).strip())+extra
    if shutil.which("brew"):
        flags=[]
        for package,library in (("json-c","json-c"),("opus","opus")):
            prefix=subprocess.check_output(["brew","--prefix",package],text=True).strip()
            flags.extend(["-L"+str(Path(prefix)/"lib"),"-l"+library])
        return flags+extra
    return ["-ljson-c", "-lopus"]+extra

@functools.cache
def opus_library():
    found=ctypes.util.find_library("opus")
    if found:
        return found
    if shutil.which("pkg-config"):
        directory=subprocess.check_output(["pkg-config", "--variable=libdir", "opus"],text=True).strip()
    elif shutil.which("brew"):
        directory=str(Path(subprocess.check_output(["brew","--prefix","opus"],text=True).strip())/"lib")
    else:
        raise RuntimeError("Install the Opus runtime library and pkg-config for native tests")
    for name in ("libopus.so", "libopus.so.0", "libopus.dylib", "opus.dll"):
        path=Path(directory)/name
        if path.is_file():
            return str(path)
    raise RuntimeError("Install the Opus runtime library for native tests")
