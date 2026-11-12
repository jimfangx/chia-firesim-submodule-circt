#!/usr/bin/env python3
"""Build the native compiler incrementally and publish one complete FireSim result.

Make invokes this check for both replace-rtl and driver. A content-bound receipt
permits reuse without rewriting headers or trusting unreceipted SFC artifacts.
"""
import argparse
import ctypes
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import shlex
import signal
import subprocess
import sys
import tempfile
import time


VERSION = 1
SUFFIXES = ('.sv', '.memory-origins.json', '.synthesis.xdc',
            '.implementation.xdc', '.defines.vh', '.const.h', '.const.vh',
            '.partition.const.h', '.peer2peer.const.yaml')


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(data)
    return h.hexdigest()


def record(path):
    path = path.resolve(strict=True)
    if not path.is_file():
        raise ValueError('not a regular file: ' + str(path))
    return dict(path=str(path), bytes=path.stat().st_size, sha256=digest(path))


def matches(item):
    try:
        return record(Path(item['path'])) == item
    except (OSError, KeyError, TypeError, ValueError, AttributeError):
        return False


@contextmanager
def locked(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('a') as stream:
        fcntl.flock(stream, fcntl.LOCK_EX)
        yield


def write_json(path, value):
    with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write('\n')
    temporary.replace(path)


def installation_identity(prefix):
    """Detect package replacement without rereading installed library bodies."""
    paths = set()
    for relative in ('include/circt', 'include/mlir', 'include/llvm', 'include/llvm-c',
                     'lib/cmake/circt', 'lib/cmake/mlir', 'lib/cmake/llvm'):
        directory = prefix / relative
        if directory.is_dir():
            paths.update(path for path in directory.rglob('*') if path.is_file() or path.is_symlink())
    for directory in (prefix / 'lib', prefix / 'lib64'):
        for library in ('CIRCT', 'MLIR', 'LLVM'):
            for ending in ('.a', '.so', '.so.*', '.dylib'):
                paths.update(directory.glob('lib' + library + '*' + ending))
    for pattern in ('lib/libz*.a', 'lib/libz*.so*', 'include/zlib.h', 'include/zconf.h'):
        paths.update(prefix.parent.glob(pattern))
    entries = []
    for path in sorted(paths):
        link = path.lstat()
        target = path.stat() if path.exists() else link
        entries.append((str(path), str(path.resolve()),
                        link.st_ino, link.st_dev, link.st_size, link.st_mtime_ns, link.st_ctime_ns,
                        target.st_ino, target.st_dev, target.st_size,
                        target.st_mtime_ns, target.st_ctime_ns))
    return dict(files=len(entries), sha256=hashlib.sha256(
        json.dumps(entries, separators=(',', ':')).encode()).hexdigest())


def compiler(options):
    # Configuration isolation prevents an existing CMake cache from silently
    # retaining a different compiler/toolchain or build type. One build is
    # shared by recipes with the same configuration, not by unrelated clones.
    cmake = record(Path(shutil.which('cmake') or 'cmake'))
    config = dict(source=str(options.source_dir.resolve()),
                  prefix=str(options.circt_prefix.resolve()), build_type=options.build_type,
                  zlib_root=str(options.circt_prefix.parent.resolve()), generator='Ninja',
                  installation=installation_identity(options.circt_prefix),
                  cxx_flags=options.cxx_flags, linker_flags=options.linker_flags,
                  cmake=cmake, environment={key: os.environ.get(key, '') for key in
                  ('CC', 'CXX', 'CMAKE_PREFIX_PATH', 'CMAKE_TOOLCHAIN_FILE')})
    build_env = dict(os.environ)
    # Driver targets export these flags to their header prerequisites. They
    # describe the FPGA driver, not the compiler executable building that header.
    for variable in ('CFLAGS', 'CXXFLAGS', 'LDFLAGS'):
        build_env.pop(variable, None)
    config['compiler_commands'] = {}
    for variable, default in (('CC', 'cc'), ('CXX', 'c++')):
        command = shlex.split(os.environ.get(variable) or default)
        selected = shutil.which(command[0])
        config['compiler_commands'][variable] = dict(
            argv=command, executable=record(Path(selected)) if selected else command[0])
    key = hashlib.sha256(json.dumps(config, sort_keys=True).encode()).hexdigest()[:24]
    build = options.build_root.resolve() / key
    with locked(options.build_root / (key + '.lock')):
        build.mkdir(parents=True, exist_ok=True)
        incomplete = build / '.goldengate-circt-build-incomplete'
        recovering = incomplete.exists()
        incomplete.write_text(str(os.getpid()) + '\n')
        if recovering or not (build / 'CMakeCache.txt').is_file():
            checked_owned(['cmake', '-S', str(options.source_dir), '-B', str(build),
                            '-G', 'Ninja', '-DCIRCT_DIR=' + str(options.circt_prefix / 'lib/cmake/circt'),
                            '-DCMAKE_BUILD_TYPE=' + options.build_type, '-DBUILD_TESTING=OFF',
                            '-DCMAKE_CXX_FLAGS=' + options.cxx_flags,
                            '-DCMAKE_EXE_LINKER_FLAGS=' + options.linker_flags,
                            '-DZLIB_ROOT=' + str(options.circt_prefix.parent.resolve())],
                           env=build_env, phase='native-configure')
        # Ninja checks source/header/library dependencies even on a receipt hit.
        # It also reconfigures when CMake input files change.
        command = ['cmake', '--build', str(build), '--target', 'goldengate-circt',
                   '--parallel', str(options.jobs)]
        if recovering:
            # Interrupted compiler workers may have left truncated object files.
            # Reconfigure/clean this cache before trusting another executable.
            command.append('--clean-first')
        checked_owned(command, env=build_env, phase='native-build')
        binary = build / 'goldengate-circt'
        identity = record(binary)
        # Never execute a binary another recipe can replace during its build.
        # Snapshots are immutable and installed atomically under the build lock.
        # Keep the same executable directory so any $ORIGIN runtime-library
        # paths continue to resolve exactly as in the CMake-built executable.
        snapshot = build / ('goldengate-circt-' + identity['sha256'])
        if not snapshot.is_file() or digest(snapshot) != identity['sha256']:
            snapshot.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(dir=snapshot.parent, delete=False) as stream:
                temporary = Path(stream.name)
            shutil.copy2(binary, temporary)
            if digest(temporary) != identity['sha256']:
                temporary.unlink()
                raise RuntimeError('compiler changed while snapshotting')
            temporary.replace(snapshot)
        if installation_identity(options.circt_prefix) != config['installation']:
            raise RuntimeError('CIRCT installation changed during compiler bootstrap')
        incomplete.unlink()
        return snapshot, dict(binary=record(snapshot), configuration=config)


def output_names(base, platform):
    names = [base + suffix for suffix in SUFFIXES]
    if platform == 'f2':
        names.append(base + '.f2-cpu-stream-protocol.json')
    names.append('post-bridge-extraction.json')
    return names


def reusable(receipt, request, output, required, base):
    try:
        old = json.loads(receipt.read_text())
        if (old['version'] != VERSION or old['backend'] != 'circt' or
                old['request'] != request or set(required) - set(old['outputs'])):
            return False
        expected = set(required) | {path.name for path in output.glob(base + '.*.ipgen.tcl')}
        protocol = base + '.f2-cpu-stream-protocol.json'
        if (output / protocol).exists():
            expected.add(protocol)
        if set(old['outputs']) != expected:
            return False
        for name, item in old['outputs'].items():
            destination = output / name
            if (Path(name).name != name or destination.is_symlink() or
                    item.get('path') != str(destination) or not matches(item)):
                return False
        listing = output / 'circt-ingestion/firrtl_black_box_resource_files.f'
        current_listing = record(listing) if listing.exists() else None
        if old['resource_listing'] != current_listing:
            return False
        paths = [str(Path(line.strip()).resolve()) for line in listing.read_text().splitlines()
                 if line.strip()] if listing.exists() else []
        if [item['path'] for item in old['resources']] != paths:
            return False
        if any(not matches(item) for item in old['resources']):
            return False
        if set(output.glob(base + '.*.ipgen.tcl')) != {
                output / name for name in old['outputs'] if name.endswith('.ipgen.tcl')}:
            return False
        return True
    except (OSError, KeyError, TypeError, ValueError, AttributeError):
        return False


def copy_atomic(source, destination):
    with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as stream:
        temporary = Path(stream.name)
        with source.open('rb') as incoming:
            shutil.copyfileobj(incoming, stream)
    shutil.copymode(source, temporary)
    temporary.replace(destination)


def build_arguments(options):
    return ['--source-dir', str(options.source_dir), '--build-root', str(options.build_root),
            '--circt-prefix', str(options.circt_prefix), '--build-type', options.build_type,
            '--jobs', str(options.jobs), '--cxx-flags=' + options.cxx_flags,
            '--linker-flags=' + options.linker_flags]


def process_state(pid):
    try:
        fields = (Path('/proc') / str(pid) / 'stat').read_text().rsplit(') ', 1)[1].split()
        return fields[19], fields[0], int(fields[1])  # start time, state, parent
    except (OSError, IndexError):
        return None


def direct_children(pid):
    result = set()
    try:
        tasks = (Path('/proc') / str(pid) / 'task').iterdir()
        for task in tasks:
            try:
                result.update(int(child) for child in (task / 'children').read_text().split())
            except FileNotFoundError:
                pass
    except FileNotFoundError:
        pass
    return result


def run_owned(commands, env=None, phases=None):
    """Join owned process trees, including Ninja's separate worker groups.

    Linux subreaping keeps orphaned descendants attached to this supervisor.
    Signals target only discovered descendants with unchanged process identities;
    unrelated process groups are never signalled. Cancellation completes before
    returning to the caller, which can therefore safely release its file locks.
    """
    if not sys.platform.startswith('linux'):
        raise RuntimeError('FireSim native build supervision requires Linux')
    libc = ctypes.CDLL(None, use_errno=True)
    previous_subreaper = ctypes.c_int()
    if libc.prctl(37, ctypes.byref(previous_subreaper), 0, 0, 0) or libc.prctl(36, 1, 0, 0, 0):
        raise OSError(ctypes.get_errno(), 'cannot enable child subreaping')
    owner = os.getpid()
    existing = set()
    for pid in direct_children(owner):
        state = process_state(pid)
        if state:
            existing.add((pid, state[0]))
    children = []
    interrupted = []

    def on_signal(signum, frame):
        interrupted.append(signum)

    def owned_tree():
        found = {}
        pending = [owner]
        while pending:
            parent = pending.pop()
            for pid in direct_children(parent):
                state = process_state(pid)
                if (not state or state[2] != parent or (pid, state[0]) in existing or pid in found):
                    continue
                found[pid] = state
                pending.append(pid)
        return found

    def reap_adopted(tree):
        failures = []
        leaders = {child.pid for child in children}
        for pid, state in tree.items():
            if pid in leaders or state[2] != owner:
                continue
            try:
                reaped, status = os.waitpid(pid, os.WNOHANG)
            except ChildProcessError:
                continue
            if reaped:
                code = os.waitstatus_to_exitcode(status)
                if code:
                    failures.append(code if code > 0 else 128 - code)
        return failures

    def stop_and_reap():
        deadline = time.monotonic() + 5
        terminated = set()
        while True:
            tree = owned_tree()
            force = time.monotonic() >= deadline
            for pid, state in reversed(list(tree.items())):
                if state[1] == 'Z' or (not force and (pid, state[0]) in terminated):
                    continue
                current = process_state(pid)
                if not current or current[0] != state[0]:
                    continue
                try:
                    os.kill(pid, signal.SIGKILL if force else signal.SIGTERM)
                    terminated.add((pid, state[0]))
                except ProcessLookupError:
                    pass
            for child in children:
                child.poll()
            reap_adopted(owned_tree())
            if all(child.returncode is not None for child in children) and not owned_tree():
                return
            time.sleep(0.02)

    signals = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)
    previous = {sig: signal.signal(sig, on_signal) for sig in signals}
    success = False
    try:
        for index, command in enumerate(commands):
            phase = phases[index] if phases else 'command'
            print('[goldengate-circt] argv phase=' + phase + ' ' + shlex.join(command), flush=True)
            children.append(subprocess.Popen(command, env=env, start_new_session=True))
        while True:
            if interrupted:
                return 128 + interrupted[0]
            statuses = [child.poll() for child in children]
            failed = next((code for code in statuses if code is not None and code != 0), None)
            if failed is not None:
                return failed if failed > 0 else 128 - failed
            if all(code == 0 for code in statuses):
                failures = reap_adopted(owned_tree())
                if failures:
                    return failures[0]
                if not owned_tree():
                    success = True
                    return 0
            time.sleep(0.05)
    finally:
        if not success:
            stop_and_reap()
        for sig, handler in previous.items():
            signal.signal(sig, handler)
        libc.prctl(36, previous_subreaper.value, 0, 0, 0)


def checked_owned(command, env=None, phase='command'):
    started = time.monotonic()
    print('[goldengate-circt] start phase=' + phase, flush=True)
    result = run_owned([command], env=env, phases=[phase])
    print('[goldengate-circt] finish phase=' + phase + ' exit=' + str(result) +
          ' wall_seconds=' + format(time.monotonic() - started, '.6f'), flush=True)
    if result:
        raise subprocess.CalledProcessError(result, command)


def with_frontend(options):
    command = options.flags[1:] if options.flags[:1] == ['--'] else options.flags
    if not command:
        raise ValueError('missing frontend command')
    bootstrap = [sys.executable, str(Path(__file__).resolve()),
                 *build_arguments(options), '--bootstrap-only']
    started = time.monotonic()
    print('[goldengate-circt] start phase=bootstrap-frontend-overlap', flush=True)
    result = run_owned([bootstrap, command], phases=['native-bootstrap', 'firechip-frontend'])
    print('[goldengate-circt] finish phase=bootstrap-frontend-overlap exit=' + str(result) +
          ' wall_seconds=' + format(time.monotonic() - started, '.6f'), flush=True)
    return result


def compile_design(options):
    output = options.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    receipt = output / '.goldengate-circt-complete.json'
    with locked(output / '.goldengate-circt.lock'):
        publisher = record(Path(__file__))
        try:
            binary, identity = compiler(options)
        except BaseException:
            receipt.unlink(missing_ok=True)
            raise
        if publisher != record(Path(__file__)):
            receipt.unlink(missing_ok=True)
            raise RuntimeError('publication helper changed during compiler bootstrap')
        flags = options.flags[1:] if options.flags[:1] == ['--'] else options.flags
        request = dict(compiler=identity, firrtl=record(options.input),
                       annotations=record(options.annotations), platform=options.platform,
                       platform_config=options.platform_config, base=options.base,
                       output_dir=str(output), flags=flags, publisher=publisher)
        required = output_names(options.base, options.platform)

        def unchanged():
            return (record(options.input) == request['firrtl'] and
                    record(options.annotations) == request['annotations'] and
                    record(binary) == request['compiler']['binary'] and
                    record(Path(__file__)) == publisher and
                    installation_identity(options.circt_prefix) == identity['configuration']['installation'])

        if reusable(receipt, request, output, required, options.base):
            if not unchanged():
                receipt.unlink(missing_ok=True)
                raise RuntimeError('compiler inputs changed while validating cached outputs')
            print('Reusing complete CIRCT RTL and driver collateral: ' + str(receipt), flush=True)
            return

        # A failed or interrupted invocation cannot leave a successful receipt.
        # Final files remain unchanged until the new compiler invocation succeeds.
        receipt.unlink(missing_ok=True)
        ingestion = output / 'circt-ingestion'
        if ingestion.is_symlink():
            raise ValueError('CIRCT ingestion directory must not be a symlink')
        if ingestion.exists():
            shutil.rmtree(ingestion)
        ingestion.mkdir()
        argv = [str(binary), str(options.input.resolve()), '--annotation-file',
                str(options.annotations.resolve()), '--output-dir', str(ingestion),
                '--compile-baseline', '--platform-config', options.platform_config,
                '--output-filename-base', options.base, *flags]
        print('Compiling CIRCT RTL and driver collateral', flush=True)
        checked_owned(argv, phase='design-compile')
        if not unchanged():
            raise RuntimeError('compiler inputs changed during compilation; no result was published')

        sources = {name: ingestion / name for name in required}
        sources['post-bridge-extraction.json'] = ingestion / 'post-bridge-extraction-all.json'
        protocol = options.base + '.f2-cpu-stream-protocol.json'
        if (ingestion / protocol).is_file():
            sources[protocol] = ingestion / protocol
        sources.update({path.name: path for path in ingestion.glob(options.base + '.*.ipgen.tcl')})
        for path in sources.values():
            if not path.is_file() or path.is_symlink():
                raise ValueError('missing compiler output: ' + str(path))
        for suffix in ('.sv', '.const.h', '.const.vh', '.memory-origins.json'):
            if not (ingestion / (options.base + suffix)).stat().st_size:
                raise ValueError('empty compiler output: ' + options.base + suffix)
        for name, path in sources.items():
            if name.endswith('.json') and not isinstance(json.loads(path.read_text()), (dict, list)):
                raise ValueError('invalid JSON compiler collateral: ' + str(path))

        resources = []
        listing = ingestion / 'firrtl_black_box_resource_files.f'
        if listing.exists():
            for line in listing.read_text().splitlines():
                if line.strip():
                    path = Path(line.strip()).resolve(strict=True)
                    if path == sources[options.base + '.sv']:
                        raise ValueError('blackbox list references simulator RTL')
                    resources.append(record(path))
        rtl = output / (options.base + '.sv')
        with tempfile.NamedTemporaryFile(dir=output, delete=False) as stream:
            temporary = Path(stream.name)
            with sources[rtl.name].open('rb') as incoming:
                shutil.copyfileobj(incoming, stream)
            for item in resources:
                with Path(item['path']).open('rb') as incoming:
                    shutil.copyfileobj(incoming, stream)
                stream.write(b'\n')
        if any(not matches(item) for item in resources):
            temporary.unlink()
            raise RuntimeError('blackbox resource changed during publication')
        shutil.copymode(sources[rtl.name], temporary)
        temporary.replace(rtl)
        for name, source in sources.items():
            if name != rtl.name:
                copy_atomic(source, output / name)
        for old in output.glob(options.base + '.*.ipgen.tcl'):
            if old.name not in sources:
                old.unlink()
        if protocol not in sources:
            (output / protocol).unlink(missing_ok=True)
        write_json(receipt, dict(version=VERSION, backend='circt', request=request,
                                argv=argv, resources=resources,
                                resource_listing=record(listing) if listing.exists() else None,
                                outputs={name: record(output / name) for name in sources}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-dir', type=Path, required=True)
    parser.add_argument('--build-root', type=Path, required=True)
    parser.add_argument('--circt-prefix', type=Path, required=True)
    parser.add_argument('--build-type', default='Release')
    parser.add_argument('--cxx-flags', default='')
    parser.add_argument('--linker-flags', default='')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 1)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--bootstrap-only', action='store_true')
    modes.add_argument('--with-frontend', action='store_true')
    parser.add_argument('--input', type=Path)
    parser.add_argument('--annotations', type=Path)
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--base')
    parser.add_argument('--platform')
    parser.add_argument('--platform-config')
    parser.add_argument('flags', nargs=argparse.REMAINDER)
    options = parser.parse_args()
    if options.jobs < 1:
        parser.error('jobs must be positive')
    options.source_dir = options.source_dir.absolute()
    options.circt_prefix = options.circt_prefix.absolute()
    options.build_root = options.build_root.absolute()
    if options.bootstrap_only:
        compiler(options)
    elif options.with_frontend:
        return with_frontend(options)
    else:
        for name in ('input', 'annotations', 'output_dir', 'base', 'platform', 'platform_config'):
            if getattr(options, name) is None:
                parser.error('missing --' + name.replace('_', '-'))
        if (not options.base or options.base in ('.', '..') or
                Path(options.base).name != options.base):
            parser.error('base must be a local filename')
        compile_design(options)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
