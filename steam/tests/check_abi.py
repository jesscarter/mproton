#!/usr/bin/env python3
"""ABI check for lsteamclient on arm64 macOS.

lsteamclient's unix side calls the native steamclient library with the struct
layouts that gen_wrapper.py computed for "u64", which upstream defines as
x86_64 Linux. On a Mac the library is the arm64 slice of steamclient.dylib, so
those layouts have to hold for arm64-apple-macos too. This script:

1. copies steam/lsteamclient to a temporary directory, runs gen_wrapper.py
   there (which also runs its own libclang arm64-apple-macos cross-check) and
   checks that the regenerated sources are identical to the committed ones;
2. turns the layouts gen_wrapper.py used into static_assert(sizeof/alignof/
   offsetof) checks against the Steamworks SDK headers and compiles them with
   the clang driver (-fsyntax-only, no SDK or sysroot needed):
     - u64 layouts on arm64-apple-macos (the macOS steamclient.dylib ABI)
     - w64 layouts on x86_64-, aarch64- and arm64ec-pc-windows-msvc (the game
       ABI; arm64ec must match x86_64 for FEX/ARM64EC games)
     - w32 layouts on i686-pc-windows-msvc (WoW64 games);
3. diff-checks Windows x86_64 against macOS arm64: every struct whose layout
   differs between the two must have a generated w64 <-> u64 converter.

Requirements: python3 with the libclang 15 bindings (pip install
libclang==15.0.6.1, the version upstream Proton generates with) and a clang
driver that knows the arm64-apple-macos and *-windows-msvc targets (any
LLVM >= 15). Exit status is 0 when every check passes.
"""

import argparse
import ast
import concurrent.futures
import filecmp
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
LSTEAMCLIENT = os.path.join(os.path.dirname(HERE), 'lsteamclient')

WINDOWS_TARGETS = [
    ('w64', 'x86_64-pc-windows-msvc'),
    ('w64', 'aarch64-pc-windows-msvc'),
    ('w64', 'arm64ec-pc-windows-msvc'),
    ('w32', 'i686-pc-windows-msvc'),
]
DARWIN_TARGET = ('u64', 'arm64-apple-macos11')


def sdk_sources():
    """SDK_SOURCES keys from gen_wrapper.py (the headers each SDK TU includes)"""
    tree = ast.parse(open(os.path.join(LSTEAMCLIENT, 'gen_wrapper.py')).read())
    for node in tree.body:
        if isinstance(node, ast.Assign) and getattr(node.targets[0], 'id', '') == 'SDK_SOURCES':
            return [k.value for k in node.value.keys]
    raise RuntimeError('SDK_SOURCES not found in gen_wrapper.py')


def regenerate(workdir, clang_resource_dir):
    src = os.path.join(workdir, 'lsteamclient')
    shutil.copytree(LSTEAMCLIENT, src, ignore=shutil.ignore_patterns('__pycache__'))
    layouts = os.path.join(workdir, 'layouts.json')
    env = dict(os.environ)
    if clang_resource_dir:
        env['CLANG_RESOURCE_DIR'] = clang_resource_dir
    proc = subprocess.run([sys.executable, 'gen_wrapper.py', '--dump-layouts', layouts],
                          cwd=src, env=env, capture_output=True, text=True)
    log = (proc.stdout + proc.stderr).replace('\r', '\n')
    for line in log.splitlines():
        if line.startswith('darwin check') or 'differ' in line or 'Error' in line:
            print('  gen_wrapper.py: ' + line)
    if proc.returncode:
        print(log[-4000:])
        raise SystemExit('gen_wrapper.py failed')

    changed = []
    cmp = filecmp.dircmp(LSTEAMCLIENT, src, ignore=['__pycache__'])
    changed += cmp.diff_files + cmp.left_only
    changed += [f for f in cmp.right_only if f != '__pycache__']
    # dircmp's shallow compare uses stat signatures; recheck by content
    changed += [f for f in cmp.same_files
                if not filecmp.cmp(os.path.join(LSTEAMCLIENT, f), os.path.join(src, f), shallow=False)]
    return json.load(open(layouts)), sorted(set(changed))


def cxx_name(name):
    return name.removeprefix('struct ').removeprefix('union ')


def checks_for(structs, abi):
    out = []
    for name, entry in sorted(structs.items()):
        if entry['exempt'] or abi not in entry:
            continue
        layout, t = entry[abi], cxx_name(name)
        out.append(f'static_assert(sizeof({t}) == {layout["size"]}, "sizeof {t}");')
        out.append(f'static_assert(alignof({t}) == {layout["align"]}, "alignof {t}");')
        for field, offset, size in layout['fields']:
            if not field:
                continue
            out.append(f'static_assert(__builtin_offsetof({t}, {field}) == {offset}, "offsetof {t}::{field}");')
            out.append(f'static_assert(sizeof((({t} *)0)->{field}) == {size}, "sizeof {t}::{field}");')
    return out


def blank_unused_headers(src):
    """gen_wrapper.py parses headers that only contain #error "This file isn't
    used any more" as empty files; do the same in the copy we compile from"""
    marker = """#error "This file isn't used any more"\n"""
    for sdkdir in os.listdir(src):
        if not sdkdir.startswith('steamworks_sdk_'):
            continue
        for name in os.listdir(os.path.join(src, sdkdir)):
            path = os.path.join(src, sdkdir, name)
            if marker in open(path, errors='replace').readlines():
                open(path, 'w').close()


def compile_checks(clang, sdkver, headers, structs, abi, target, workdir):
    src = os.path.join(workdir, 'lsteamclient')
    sdkdir = f'steamworks_sdk_{sdkver}'
    lines = [f'#if __has_include("{sdkdir}/{h}")\n#include "{sdkdir}/{h}"\n#endif' for h in headers]
    lines += checks_for(structs, abi)
    path = os.path.join(workdir, f'check_{sdkver}_{target}.cpp')
    with open(path, 'w') as f:
        f.write('\n'.join(lines) + '\n')

    args = [clang, '-target', target, '-x', 'c++', '-std=c++17', '-fsyntax-only',
            '-nostdlibinc', '-nostdinc++', '-I', os.path.join(src, 'gen_include'), '-I', src, '-Dprivate=public', '-Dprotected=public',
            '-Wno-invalid-offsetof', '-Wno-everything', '-ferror-limit=0', path]
    if 'apple' in target:
        args += ['-DGNUC']
    proc = subprocess.run(args, capture_output=True, text=True)
    errors = [l for l in proc.stderr.splitlines() if 'error:' in l]
    return sdkver, target, proc.returncode, errors, len(lines) - len(headers)


def field_map(layout):
    return {name: (offset, size) for name, offset, size in layout['fields'] if name}


def windows_vs_darwin(dump):
    """structs whose x86_64-windows and arm64-darwin layouts differ"""
    differ, unconverted = [], []
    for sdkver, structs in dump['sdks'].items():
        for name, entry in structs.items():
            if entry['exempt'] or 'd64' not in entry:
                continue
            w, d = field_map(entry['w64']), field_map(entry['d64'])
            d = {k: v for k, v in d.items() if k in w}
            if w == d:
                continue
            differ.append((sdkver, name))
            if not entry['w64_u64_conv']:
                unconverted.append((sdkver, name))
    return differ, unconverted


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--clang', default=os.environ.get('CLANG', 'clang'),
                        help='clang driver used for the static_assert checks')
    parser.add_argument('--clang-resource-dir', default=os.environ.get('CLANG_RESOURCE_DIR'),
                        help='builtin headers for gen_wrapper.py (default: clang -print-resource-dir)')
    parser.add_argument('--keep', action='store_true', help='keep the temporary directory')
    parser.add_argument('--report', metavar='FILE',
                        help='write the Windows x86_64 vs macOS arm64 differences as Markdown')
    args = parser.parse_args()

    workdir = tempfile.mkdtemp(prefix='lsteamclient-abi-')
    failed = False
    try:
        print('[1/3] regenerating the wrappers')
        dump, changed = regenerate(workdir, args.clang_resource_dir)
        if changed:
            failed = True
            print('  FAIL: regenerated sources differ from the committed ones: ' + ', '.join(changed[:20]))
        else:
            print('  ok: regenerated sources are identical to the committed ones')

        print('[2/3] static_assert checks with ' + args.clang)
        blank_unused_headers(os.path.join(workdir, 'lsteamclient'))
        headers = sdk_sources()
        jobs = [(sdkver, abi, target) for sdkver in dump['sdks']
                for abi, target in [DARWIN_TARGET] + WINDOWS_TARGETS]
        results = {}
        with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
            futures = [pool.submit(compile_checks, args.clang, sdkver, headers, dump['sdks'][sdkver],
                                   abi, target, workdir) for sdkver, abi, target in jobs]
            for fut in concurrent.futures.as_completed(futures):
                sdkver, target, rc, errors, count = fut.result()
                ok, total = results.get(target, (0, 0))
                results[target] = (ok + (rc == 0), total + count)
                if rc:
                    failed = True
                    print(f'  FAIL: sdk {sdkver} on {target}:')
                    for e in errors[:10]:
                        print('    ' + e)
        for abi, target in [DARWIN_TARGET] + WINDOWS_TARGETS:
            ok, total = results[target]
            print(f'  {target:26} {abi} layouts: {ok}/{len(dump["sdks"])} SDKs pass, {total} assertions')

        print('[3/3] x86_64-windows <-> arm64-darwin diff check')
        differ, unconverted = windows_vs_darwin(dump)
        names = sorted({n for _, n in differ})
        print(f'  {len(differ)} struct versions ({len(names)} structs) differ between '
              'x86_64-windows and arm64-darwin')
        if unconverted:
            failed = True
            for sdkver, name in unconverted:
                print(f'  FAIL: {name} (sdk {sdkver}) differs but has no w64<->u64 converter')
        else:
            print('  ok: every one of them has a generated w64 <-> u64 converter')

        if args.report:
            with open(args.report, 'w') as f:
                f.write('| struct | SDK versions where x86_64-windows != arm64-darwin |\n|---|---|\n')
                for n in names:
                    vers = sorted(v for v, m in differ if m == n)
                    f.write(f'| `{n}` | {", ".join(vers)} |\n')
    finally:
        if args.keep:
            print('kept ' + workdir)
        else:
            shutil.rmtree(workdir, ignore_errors=True)

    print('FAILED' if failed else 'all ABI checks passed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
