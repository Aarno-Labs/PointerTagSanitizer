#!/usr/bin/env python3
"""Check the CMake-built musl baseline and Clang's default system glibc baseline."""
import os
from pathlib import Path
import subprocess


def main():
    build = Path(os.environ['BUILD_DIR']).resolve()
    source = Path(os.environ['GITHUB_WORKSPACE']).resolve()
    results = source / 'ci-results/performance/dependencies'
    results.mkdir(parents=True, exist_ok=True)
    glibc_only = os.environ.get('PERF_GROUP') == 'glibc-stack-off'
    if not glibc_only:
        baseline = build / 'ptsan/musl-baseline'
        # CMake owns the recipe and compiler configs. CI only audits its output.
        for origin, name in ((baseline / 'libc-build/config.mak', 'musl-config.mak'),
                             (baseline / 'cxx-build/compile_commands.json', 'musl-compile_commands.json'),
                             (baseline / 'cxx-build/CMakeCache.txt', 'musl-CMakeCache.txt')):
            text = origin.read_text()
            if name != 'musl-CMakeCache.txt' and ('-fsanitize=ptsan' in text or '-ptsan-' in text):
                raise ValueError(f'Instrumented baseline dependency: {origin}')
            (results / name).write_text(text)

    smoke = results / 'baseline-smoke.cpp'
    smoke.write_text('#include <iostream>\n#include <vector>\nint main() { std::vector<int> v{1,2,3}; '
                     'try { throw v.at(2); } catch (int n) { std::cout << n << "\\n"; return n != 3; } }\n')
    smoke_dir = build / 'baseline-smoke'
    smoke_dir.mkdir(exist_ok=False)
    libcs = ('glibc',) if glibc_only else ('glibc', 'musl')
    for libc in libcs:
        exe = smoke_dir / libc
        # No config, sysroot, -stdlib, custom headers, library paths, or rpath
        # for glibc: measure our Clang against the default system libraries.
        flags = [] if libc == 'glibc' else ['--config=ptsan-musl-baseline.cfg']
        command = [str(build / 'bin/clang++'), *flags, '-O2', str(smoke), '-o', str(exe)]
        invocation = subprocess.run([*command, '-###'], text=True, capture_output=True, check=True)
        (results / f'{libc}-compiler-command.txt').write_text(invocation.stderr)
        subprocess.run(command, check=True)
        subprocess.run([str(exe)], check=True)
        headers = subprocess.check_output(['readelf', '-l', str(exe)], text=True)
        if ('INTERP' in headers) != (libc == 'glibc'):
            raise ValueError(f'Incorrect libc linkage: {libc}')
        (results / f'{libc}-elf.txt').write_text(headers)
        symbols = subprocess.check_output(['nm', str(exe)], text=True)
        if '__ptsan_' in symbols:
            raise ValueError(f'PTSan runtime linked into baseline: {libc}')
        if libc == 'glibc':
            libraries = subprocess.check_output(['ldd', str(exe)], text=True)
            (results / 'glibc-system-libraries.txt').write_text(libraries)


if __name__ == '__main__':
    main()
