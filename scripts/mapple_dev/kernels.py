"""Independent, copied kernel slots and parameterized Linux builds."""
from contextlib import ExitStack
import json
import os
from pathlib import Path
import re
import shutil
import shlex
import subprocess
import tarfile
import urllib.request
import uuid

from . import core as c

ARCHES = {'x86_64': ('x86', 'x86_64_defconfig', 'arch/x86/boot/bzImage', 'x64'),
          'aarch64': ('arm64', 'defconfig', 'arch/arm64/boot/Image', 'arm64'),
          'arm': ('arm', 'multi_v7_defconfig', 'arch/arm/boot/zImage', 'arm'),
          'riscv64': ('riscv', 'defconfig', 'arch/riscv/boot/Image', 'riscv64')}


def normalize(arch):
    return {'amd64': 'x86_64', 'arm64': 'aarch64', 'armv7l': 'arm', 'i686': 'i386'}.get(arch, arch)


def root(k):
    return c.record('kernels', k['name']).parent


def path(k, key):
    value = k.get('artifacts', {}).get(key)
    return root(k) / value if value else None


def toolenv(k):
    t = k.get('toolchain', {})
    return dict(os.environ, **t.get('env', {}))


def toolchain(arch):
    data = c.load(Path(c.option('TOOLCHAIN')).expanduser().resolve(strict=True)) if c.option('TOOLCHAIN') else {}
    if not isinstance(data, dict):
        c.fail('TOOLCHAIN must contain a JSON object.')
    prefix = c.option('CROSS_COMPILE', data.get('cross_compile', ''))
    if not isinstance(prefix, str):
        c.fail('TOOLCHAIN cross_compile must be a string.')
    compiler = c.option('CC', data.get('cc') or ('clang' if c.option('LLVM', str(int(bool(data.get('llvm'))))) == '1' else prefix + 'gcc' if prefix else ('gcc-12' if arch == 'x86_64' else 'gcc')))
    if not isinstance(compiler, str) or not compiler.strip():
        c.fail('TOOLCHAIN cc must name a compiler command.')
    data.update(cc=compiler, cross_compile=prefix, llvm=c.option('LLVM', str(int(bool(data.get('llvm'))))) == '1')
    if not isinstance(data.get('env', {}), dict) or not all(isinstance(v, str) for v in data.get('env', {}).values()):
        c.fail('TOOLCHAIN env must be a JSON object of string values.')
    return data


def binary_tool(k, tool):
    settings = k.get('toolchain', {})
    return ('llvm-' if settings.get('llvm') else settings.get('cross_compile', '')) + tool


def make_args(k):
    t = k.get('toolchain', {})
    return [f'ARCH={k["linux_arch"]}', f'CC={t.get("cc", "gcc")}',
            f'CROSS_COMPILE={t.get("cross_compile", "")}'] + (['LLVM=1'] if t.get('llvm') else [])


def elf_info(file):
    if not file or not file.is_file():
        return {}
    try:
        headers = c.run(['readelf', '-h', '-S', '-n', file], capture_output=True).stdout.decode(errors='replace')
    except subprocess.CalledProcessError:
        return {}
    identity = re.search(r'Build ID: ([0-9a-f]+)', headers)
    machine = re.search(r'Machine:\s+(.+)', headers)
    kind = re.search(r'Type:\s+(\w+)', headers)
    return {'build_id': identity[1] if identity else None, 'symbols': '.symtab' in headers,
            'dwarf': '.debug_info' in headers or '.zdebug_info' in headers,
            'machine': machine[1].strip() if machine else '', 'type': kind[1] if kind else ''}


def notes_id(data):
    import struct
    for order in ('<', '>'):
        offset = 0
        while offset + 12 <= len(data):
            namesz, descsz, kind = struct.unpack_from(order + 'III', data, offset)
            if namesz > len(data) or descsz > len(data):
                break
            start = offset + 12
            desc = start + ((namesz + 3) & ~3)
            end = desc + descsz
            if end > len(data):
                break
            if data[start:start + namesz].rstrip(b'\0') == b'GNU' and kind == 3:
                return data[desc:end].hex()
            offset = desc + ((descsz + 3) & ~3)
    return None


def debug_elf(k):
    return path(k, 'debug_info') or path(k, 'vmlinux')


def characteristics(k):
    build = path(k, 'build')
    source = path(k, 'source')
    elf = elf_info(debug_elf(k))
    blockers = []
    if not build:
        blockers.append('prepared build inputs missing')
    else:
        for item in ('Makefile', 'scripts/Makefile.build', 'include/config/kernel.release', 'include/generated/autoconf.h'):
            if not (build / item).is_file():
                blockers.append('missing ' + item)
        config = (build / 'include/generated/autoconf.h')
        if config.is_file() and re.search(r'#define CONFIG_MODVERSIONS 1', config.read_text()):
            if not (build / 'Module.symvers').is_file():
                blockers.append('Module.symvers required by CONFIG_MODVERSIONS')
    tier = 0 if blockers else 1
    symbols = bool(elf.get('symbols') or path(k, 'system_map'))
    if tier and symbols:
        tier = 2
        if elf.get('dwarf') and source and (source / 'init/main.c').is_file():
            tier = 3
    return {'tier': tier, 'build': not blockers, 'symbols': symbols,
            'dwarf': bool(elf.get('dwarf')), 'source': bool(source and (source / 'init/main.c').is_file()),
            'installable': bool(path(k, 'image') and path(k, 'modules')),
            'blockers': blockers, 'build_id': elf.get('build_id') or k.get('expected_build_id')}


def input_hashes(k):
    result = {}
    seen_trees = set()
    kernel_root = root(k)
    prefix_length = len(str(kernel_root)) + 1
    cache_file = kernel_root / 'verification-cache.json'
    cache = c.load(cache_file) if cache_file.exists() else {}
    fresh_cache = {}
    def hashed(p, key):
        st = p.stat()
        stamp = [st.st_size, st.st_mtime_ns, st.st_ctime_ns, st.st_ino]
        old = cache.get(key, {})
        value = old.get('digest') if old.get('stamp') == stamp else c.digest(p)
        fresh_cache[key] = {'stamp': stamp, 'digest': value}
        return value
    for key in ('build', 'source'):
        tree = path(k, key)
        if not tree or tree in seen_trees:
            continue
        seen_trees.add(tree)
        # ABI definitions and source text are sealed. Ignore mutable Kbuild
        # bookkeeping/object files, which do not redefine the kernel build.
        for directory, subdirs, files in os.walk(tree):
            subdirs.sort()
            for filename in sorted(files):
                if (Path(filename).suffix in ('.h', '.c', '.S', '.rs', '.sh', '.py', '.pl', '.awk', '.lds', '.seed') or
                        filename.startswith(('Makefile', 'Kconfig')) or
                        filename in ('.config', 'Module.symvers', 'kernel.release', 'auto.conf', 'modpost', 'objtool', 'fixdep')):
                    p = Path(directory) / filename
                    if p.is_file():
                        key = str(p)[prefix_length:]
                        result[key] = hashed(p, key)
    for key in ('vmlinux', 'debug_info', 'config', 'btf', 'system_map', 'image', 'modules'):
        p = path(k, key)
        if p and p.is_file():
            key = str(p)[prefix_length:]
            result[key] = hashed(p, key)
    if fresh_cache != cache:
        c.save(cache_file, fresh_cache)
    return result


def seal(k):
    infos = []
    for key in ('vmlinux', 'debug_info'):
        file = path(k, key)
        if file:
            info = elf_info(file)
            if not info:
                c.fail(key + ' must be a valid ELF file, not a boot image or BTF blob.')
            infos.append(info)
    identities = {info['build_id'] for info in infos if info.get('build_id')}
    if k.get('expected_build_id'):
        identities.add(k['expected_build_id'])
    if len(identities) > 1:
        c.fail('Kernel ELF/debug build IDs do not match this slot.')
    for info in infos:
        expected = {'x86_64': 'X86-64', 'aarch64': 'AArch64', 'arm': 'ARM', 'riscv64': 'RISC-V'}.get(k['arch'])
        if expected and expected not in info.get('machine', ''):
            c.fail('ELF architecture differs from the kernel slot.')
        if info.get('type') not in ('EXEC', 'DYN'):
            c.fail('Linux debug files must describe a linked kernel ELF, not an external module/object file.')
    k['capabilities'] = characteristics(k)
    build = path(k, 'build')
    if build and (build / 'include/config/kernel.release').exists():
        release = (build / 'include/config/kernel.release').read_text().strip()
        if k.get('release') and release != k['release']:
            c.fail('Kernel release differs from this slot. Create another kernel slot.')
        k['release'] = release
    k['hashes'] = input_hashes(k)
    k['sealed_at'] = c.now()
    k['state'] = 'ready' if k['capabilities']['build'] else 'incomplete'
    c.save(c.record('kernels', k['name']), k)
    return k


def validate(k):
    actual = input_hashes(k)
    if actual != k.get('hashes', {}):
        c.fail(f'Kernel {k["name"]} files changed after sealing. Restore them or create a new slot.')


def create():
    name = c.name(c.option('NAME'))
    version = c.option('VERSION', '6.6.6')
    if not re.fullmatch(r'\d+\.\d+(?:\.\d+)?', version):
        c.fail('VERSION must be a Linux release such as 6.6.6.')
    arch = normalize(c.option('ARCH', 'x86_64'))
    defaults = ARCHES.get(arch)
    linux_arch = c.option('LINUX_ARCH', defaults[0] if defaults else arch)
    image_path = c.option('IMAGE_PATH', defaults[2] if defaults else None)
    if not defaults and (not c.option('CONFIG') or not image_path):
        c.fail('For this architecture, supply CONFIG, LINUX_ARCH and IMAGE_PATH explicitly.')
    if Path(image_path).is_absolute() or '..' in Path(image_path).parts:
        c.fail('IMAGE_PATH must stay inside the kernel source tree.')
    if c.option('PROFILE', 'debug') != 'debug':
        c.fail('PROFILE must be debug; use CONFIG for a custom configuration.')
    with c.lock('catalog'):
        if c.record('kernels', name).exists():
            c.fail('Kernel name already exists. Use a new NAME.')
        k = {'name': name, 'id': uuid.uuid4().hex, 'origin': 'built', 'state': 'defined',
             'arch': arch, 'linux_arch': linux_arch, 'recipe': {'version': version, 'profile': 'debug',
                 'defconfig': c.option('DEFCONFIG', defaults[1] if defaults else ''), 'image_path': image_path},
             'toolchain': toolchain(arch),
             'artifacts': {}, 'created_at': c.now(), 'signer': c.option('SIGNER')}
        config = c.option('CONFIG')
        if config:
            destination = c.directory(root(k) / 'recipe') / 'config'
            shutil.copyfile(Path(config).expanduser().resolve(strict=True), destination)
            k['recipe']['config'] = str(destination.relative_to(root(k)))
        patches = []
        if c.option('PATCHES'):
            patch_dir = Path(c.option('PATCHES')).expanduser().resolve(strict=True)
            files = sorted(patch_dir.glob('*.patch'))
            if not files:
                c.fail('PATCHES directory contains no *.patch files.')
            for i, p in enumerate(files):
                destination = c.directory(root(k) / 'recipe/patches') / f'{i:03d}.patch'
                shutil.copyfile(p, destination)
                patches.append(str(destination.relative_to(root(k))))
        k['recipe']['patches'] = patches
        c.save(c.record('kernels', name), k)
    print(f'Defined {name}. Build with make kernel-build KERNEL={name}.')


def build(k):
    with c.lock('kernel-' + k['name'], wait=3600):
        current = c.get('kernels', k['name'])
        if current['id'] != k['id']:
            c.fail('Kernel slot was replaced while waiting for its build.')
        k = current
        if k.get('sealed_at'):
            validate(k)
            if not k.get('capabilities', {}).get('build'):
                c.fail('Tier 0 has no usable build environment. Add matching inputs with kernel-enrich; use kernel-create for a new Linux build recipe.')
            print(f'Kernel {k["name"]} already built/imported; inputs unchanged.')
            return k
        if k['origin'] != 'built':
            c.fail('This is an imported/observed kernel; add matching inputs with kernel-enrich.')
        c.executable(shlex.split(k['toolchain']['cc'])[0], env=toolenv(k))
        source = root(k) / 'source'
        version = k['recipe']['version']
        base = f'https://cdn.kernel.org/pub/linux/kernel/v{version.split(".")[0]}.x'
        archive = c.directory(c.STATE / 'downloads') / f'linux-{version}.tar.xz'
        k['linux_toolchain'] = dict(k['toolchain'])
        k['state'] = 'building'
        c.save(c.record('kernels', k['name']), k)
        try:
            if not source.exists():
                print(f'Fetching Linux {version}…', flush=True)
                sums = urllib.request.urlopen(base + '/sha256sums.asc', timeout=60).read().decode()
                expected = next((line.split()[0] for line in sums.splitlines()
                                 if len(line.split()) == 2 and line.split()[1] == archive.name), None)
                if not expected:
                    c.fail('Kernel release missing from upstream checksums.')
                if not archive.exists() or c.digest(archive) != expected:
                    with urllib.request.urlopen(base + '/' + archive.name, timeout=120) as response, open(archive, 'wb') as out:
                        shutil.copyfileobj(response, out)
                if c.digest(archive) != expected:
                    c.fail('Kernel archive checksum mismatch.')
                shutil.rmtree(root(k) / 'unpack', ignore_errors=True)
                unpack = c.directory(root(k) / 'unpack')
                with tarfile.open(archive) as tar:
                    tar.extractall(unpack, filter='data')
                prepared = unpack / f'linux-{version}'
                for patch in k['recipe']['patches']:
                    c.run(['patch', '-p1', '--forward', '-i', root(k) / patch], cwd=prepared)
                prepared.rename(source)
                shutil.rmtree(unpack)
                k['recipe']['source_sha256'] = expected
            def make(*args):
                c.run(['make', '-C', source, *make_args(k), *args], env=toolenv(k))
            if k['recipe'].get('config'):
                shutil.copyfile(root(k) / k['recipe']['config'], source / '.config')
            else:
                make(k['recipe']['defconfig'])
            options = [str(source / 'scripts/config'), '--file', str(source / '.config')]
            if not k['recipe'].get('config'):
                for key in ('DRM', 'SOUND', 'MEDIA_SUPPORT', 'WLAN', 'BT', 'NFC'):
                    options += ['--disable', key]
            for key in ('LOCALVERSION_AUTO', 'DEBUG_INFO_NONE', 'DEBUG_INFO_BTF'):
                options += ['--disable', key]
            options += ['--set-str', 'LOCALVERSION', f'-mapple-{k["name"][:24]}-{k["id"][:8]}']
            for key in ('DEBUG_KERNEL', 'DEBUG_INFO', 'DEBUG_INFO_DWARF4', 'GDB_SCRIPTS', 'FRAME_POINTER',
                        'BLK_DEV_INITRD', 'DEVTMPFS', 'DEVTMPFS_MOUNT', 'VIRTIO', 'VIRTIO_PCI',
                        'VIRTIO_BLK', 'VIRTIO_NET', 'EXT4_FS', 'MODULES', 'KALLSYMS', 'KALLSYMS_ALL',
                        'ISO9660_FS', 'RD_ZSTD', 'RD_GZIP', 'RD_XZ', 'IKCONFIG', 'IKCONFIG_PROC',
                        'CGROUPS', 'NAMESPACES', 'SECCOMP', 'SECCOMP_FILTER'):
                options += ['--enable', key]
            c.run(options)
            make('olddefconfig')
            c.logged(['make', '-C', source, *make_args(k), '-j' + c.option('JOBS', str(min(os.cpu_count() or 2, 4))), 'all'],
                     root(k) / 'build.log', env=toolenv(k))
            release = (source / 'include/config/kernel.release').read_text().strip()
            stage = c.directory(root(k) / 'install')
            make('INSTALL_MOD_STRIP=1', f'INSTALL_MOD_PATH={stage}', 'modules_install')
            modules = root(k) / 'modules.tar.gz'
            c.run(['tar', '-C', stage / 'lib/modules', '-czf', modules, release])
            shutil.rmtree(stage)
            k['artifacts'] = {'build': 'source', 'source': 'source', 'vmlinux': 'source/vmlinux',
                              'image': 'source/' + k['recipe']['image_path'], 'modules': 'modules.tar.gz',
                              'system_map': 'source/System.map'}
            k['release'] = release
            k = seal(k)
        except BaseException as error:
            k['state'] = 'failed'
            k['last_error'] = str(error)
            print('Kernel build incomplete; retry kernel-build with JOBS=1 or JOBS=2. Completed objects are retained.')
            c.save(c.record('kernels', k['name']), k)
            raise
    print(f'Kernel {k["name"]}: {k["release"]}, tier {k["capabilities"]["tier"]}.')
    return k


def add_artifacts(k):
    mappings = {'BUILD': 'build', 'SOURCE': 'source', 'VMLINUX': 'vmlinux',
                'DEBUG_INFO': 'debug_info', 'BTF': 'btf', 'CONFIG': 'config', 'SYSTEM_MAP': 'system_map', 'IMAGE': 'image', 'MODULES': 'modules'}
    additions = {}
    originals = {}
    copied = {}
    for env, key in mappings.items():
        value = c.option(env)
        if not value:
            continue
        if key in k.get('artifacts', {}):
            c.fail(f'{key} is already present; kernel slots cannot replace existing artifacts.')
        original = Path(value).expanduser().resolve(strict=True)
        originals[key] = str(original)
        if original.is_dir() != (key in ('build', 'source')):
            c.fail(f'{env} must be a ' + ('directory' if key in ('build', 'source') else 'file'))
        if original in copied:
            additions[key] = copied[original]
            continue
        inside = next(((src, dst) for src, dst in copied.items() if src.is_dir() and original.is_relative_to(src)), None)
        if inside:
            additions[key] = str(Path(inside[1]) / original.relative_to(inside[0]))
            continue
        destination = root(k) / ('inputs-' + uuid.uuid4().hex[:8]) / key
        c.directory(destination.parent)
        if original.is_dir():
            closure = c.copy_tree(original, destination, source_only=key == 'source')
            copied.update({old: str(new.relative_to(root(k))) for old, new in closure.items()})
        else:
            shutil.copyfile(original, destination)
        copied[original] = str(destination.relative_to(root(k)))
        additions[key] = copied[original]
    candidate = dict(k, artifacts=dict(k.get('artifacts', {}), **additions))
    build = path(candidate, 'build')
    if build and (build / 'include/config/kernel.release').is_file():
        release = (build / 'include/config/kernel.release').read_text().strip()
        if k.get('release') and k['release'] != release:
            c.fail('Imported build release does not match this kernel record.')
        candidate['release'] = release
    info = elf_info(debug_elf(candidate))
    expected = k.get('capabilities', {}).get('build_id') or k.get('expected_build_id')
    if expected and info.get('build_id') and expected != info['build_id']:
        c.fail('Debug ELF build ID does not match this kernel slot.')
    if 'source' in additions and not (path(candidate, 'source') / 'init/main.c').is_file():
        c.fail('SOURCE must contain a Linux source tree (init/main.c).')
    candidate.setdefault('provenance', []).append({'at': c.now(), 'source': 'operator import',
                                                 'artifacts': additions, 'originals': originals})
    return seal(candidate)


def import_kernel():
    name = c.name(c.option('NAME'))
    with c.lock('catalog'), c.lock('kernel-' + name):
        if c.record('kernels', name).exists():
            c.fail('Kernel name already exists.')
        arch = normalize(c.option('ARCH', 'x86_64'))
        k = {'name': name, 'id': uuid.uuid4().hex, 'origin': 'imported', 'arch': arch,
             'linux_arch': c.option('LINUX_ARCH', ARCHES.get(arch, (arch,))[0]),
             'toolchain': toolchain(arch), 'artifacts': {}, 'created_at': c.now(), 'expected_build_id': c.option('BUILD_ID'), 'signer': c.option('SIGNER')}
        try:
            k = add_artifacts(k)
        except BaseException:
            shutil.rmtree(root(k), ignore_errors=True)
            raise
    print(f'Imported {name}: tier {k["capabilities"]["tier"]}.')


def enrich(k):
    with c.lock('kernel-' + k['name']):
        current = c.get('kernels', k['name'])
        if current['id'] != k['id']:
            c.fail('Kernel slot changed while waiting for enrichment.')
        k = current
        if any(c.option(key) for key in ('CC', 'CROSS_COMPILE', 'LLVM', 'TOOLCHAIN', 'SIGNER')):
            previous = k.get('toolchain', {})
            updated = toolchain(k['arch']) if c.option('TOOLCHAIN') else dict(previous)
            for key, option in (('cc', 'CC'), ('cross_compile', 'CROSS_COMPILE')):
                if c.option(option):
                    updated[key] = c.option(option)
            if c.option('LLVM'):
                updated['llvm'] = c.option('LLVM') == '1'
            k['toolchain'] = updated
            if c.option('SIGNER'):
                k['signer'] = c.option('SIGNER')
            k.setdefault('provenance', []).append({'at': c.now(), 'host_toolchain': updated, 'previous': previous})
        if k.get('hashes'):
            validate(k)
        k = add_artifacts(k)
    print(f'{k["name"]}: tier {k["capabilities"]["tier"]}.')
    return k


def selected():
    value = c.option('KERNEL')
    if not value:
        m = c.machine()
        value = m.get('kernel')
        if not value:
            c.fail('Machine has no assigned kernel. Run machine-adopt or machine-prepare.')
    return c.get('kernels', value)


def remove(k):
    with c.lock('catalog'), c.lock('kernel-' + k['name']):
        refs = [m for m in c.records('machines') if k['name'] in (m.get('kernel'), m.get('requested_kernel'))]
        if refs and c.option('CASCADE') != '1':
            c.fail('Kernel is referenced by: ' + ', '.join(m['name'] for m in refs) + '. Reassign them or use CASCADE=1.')
        from . import machines
        if any(r.get('kernel') == k['name'] and r.get('status') == 'debugging' for r in c.records('runs')):
            c.fail('Detach active debug runs before removing this kernel.')
        with ExitStack() as locks:
            # Acquire every referenced machine before retiring any of them.
            # A busy second machine must not leave a half-completed cascade.
            for m in sorted(refs, key=lambda item: item['name']):
                locks.enter_context(c.lock('machine-' + m['name']))
            for m in refs:
                machines.retire(m)
            shutil.rmtree(root(k))
    print('Removed kernel ' + k['name'])
