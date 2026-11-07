#!/usr/bin/env python3
"""Use native-host Clang with the verified OHOS API20 sysroot and runtimes."""
import argparse
from pathlib import Path
import shlex

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('sdk', type=Path)
p.add_argument('output', type=Path)
p.add_argument('--prefix', type=Path, required=True, help='Cross dependency staging root')
p.add_argument('--platform-root', type=Path, help='Read-only original system root for std::__h ABI')
a = p.parse_args()
sdk, out, prefix = a.sdk.resolve(), a.output.resolve(), a.prefix.resolve()
assert (sdk / 'sysroot/usr/lib/aarch64-linux-ohos/libc.so').is_file()
out.mkdir(parents=True, exist_ok=True)
prefix.mkdir(parents=True, exist_ok=True)
common = ['--target=aarch64-linux-ohos', '--sysroot=' + str(sdk / 'sysroot'),
          '-resource-dir=' + str(sdk / 'llvm/lib/clang/15.0.4')]
link_flags = ['-fuse-ld=lld', '-rtlib=compiler-rt',
              '-L' + str(sdk / 'llvm/lib/aarch64-linux-ohos'),
              '-Wl,-z,max-page-size=65536']
for name, compiler, flags in [
    ('ohos-cc', 'clang-19', []),
    ('ohos-cxx', 'clang++-19', ['-stdlib=libc++', '-nostdinc++',
                             '-isystem', str(sdk / 'llvm/include/libcxx-ohos/include/c++/v1')])]:
    script = out / name
    script.write_text('''#!/bin/sh
compile_only=false
for arg do
    case "$arg" in -c|-E|-S|-fsyntax-only|--print-search-dirs|-dumpmachine) compile_only=true;; esac
done
if "$compile_only"; then
    exec ''' + shlex.join([compiler, *common, *flags]) + ''' "$@"
else
    exec ''' + shlex.join([compiler, *common, *flags, *link_flags]) + ''' "$@"
fi
''')
    script.chmod(0o755)
if a.platform_root:
    platform_library = a.platform_root.resolve() / 'system/lib64/libc++.so'
    assert platform_library.is_file()
    override = out / 'platform-cxx-headers'
    override.mkdir(exist_ok=True)
    site = (sdk / 'llvm/include/libcxx-ohos/include/c++/v1/__config_site').read_text()
    assert '#define _LIBCPP_ABI_NAMESPACE __n1' in site
    (override / '__config_site').write_text(site.replace('#define _LIBCPP_ABI_NAMESPACE __n1',
                                                       '#define _LIBCPP_ABI_NAMESPACE __h'))
    headers = ['-stdlib=libc++', '-nostdlib++', '-nostdinc++', '-isystem', str(override),
               '-isystem', str(sdk / 'llvm/include/libcxx-ohos/include/c++/v1')]
    driver = out / 'ohos-platform-cxx'
    driver.write_text('''#!/bin/sh
compile_only=false
for arg do
    case "$arg" in -c|-E|-S|-fsyntax-only|--print-search-dirs|-dumpmachine) compile_only=true;; esac
done
if "$compile_only"; then
    exec ''' + shlex.join(['clang++-19', *common, *headers]) + ''' "$@"
else
    exec ''' + shlex.join(['clang++-19', *common, *headers, *link_flags]) + ' "$@" ' + shlex.quote(str(platform_library)) + '''
fi
''')
    driver.chmod(0o755)
cross = f'''[binaries]
c = '{out / 'ohos-cc'}'
cpp = '{out / 'ohos-cxx'}'
ar = 'llvm-ar-19'
strip = 'llvm-strip-19'
pkg-config = 'pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'

[properties]
needs_exe_wrapper = true
sys_root = '{prefix}'
pkg_config_libdir = ['{prefix}/system/lib64/aurora-mesa/pkgconfig', '{prefix}/system/share/pkgconfig']

[built-in options]
c_args = ['-I{prefix}/system/include']
cpp_args = ['-I{prefix}/system/include']
c_link_args = ['-L{prefix}/system/lib64/aurora-mesa', '-Wl,-rpath,/system/lib64/aurora-mesa']
cpp_link_args = ['-L{prefix}/system/lib64/aurora-mesa', '-Wl,-rpath,/system/lib64/aurora-mesa']
'''
(out / 'ohos-cross.ini').write_text(cross)
print(out)
