#!/usr/bin/python3
"""Keep Bazel's host tools native; apply a generated config to target actions."""
import json
import os
from pathlib import Path
import shlex
import sys

TARGET = '-DPTSAN_FLEETBENCH_TARGET'


def command(config, arguments):
    args = []
    for arg in arguments:
        if arg.startswith('@'):
            args.extend(shlex.split(Path(arg[1:]).read_text()))
        else:
            args.append(arg)
    compiler = config['build'] + '/bin/clang'
    extra = ['--no-default-config']
    if TARGET in args:
        args.remove(TARGET)
        extra += config['flags']
        if config['mode'] != 'plain-glibc':
            args = [arg for arg in args if arg != '-lstdc++']
        if not any(arg in args for arg in ('-c', '-E', '-S', '-fsyntax-only')):
            compiler += '++'
            # Abseil's stack walker needs the unwinder even with exceptions off.
            extra += ['-Wl,-u,_Unwind_GetIPInfo', '-lpthread', '-ldl', '-lrt']
    return [compiler, *extra, *args]


if __name__ == '__main__':
    config = json.loads(Path(sys.argv[1]).read_text())
    argv = command(config, sys.argv[2:])
    os.execv(argv[0], argv)
