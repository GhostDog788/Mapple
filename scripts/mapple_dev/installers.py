"""Explicit installation transactions. Unknown boot systems use a local hook."""
import re
import shlex
import subprocess
from . import core as c, kernels, machines


def grub_entry(text, release):
    entries = list(re.finditer(r"(?m)^([\t ]*)menuentry '[^']*Linux " + re.escape(release) + r"(?:[^']*)'[^\n]* '([^']+)'\s*\{", text))
    if not entries:
        c.fail('Installed kernel is missing from GRUB: ' + release)
    entry = entries[0]
    parents = list(re.finditer(r"(?m)^submenu '[^']+'[^\n]* '([^']+)'\s*\{", text[:entry.start()]))
    return (parents[-1][1] + '>' if entry[1] and parents else '') + entry[2]


def hook(m, k, operation):
    c.run([m['installer'], operation, c.record('machines', m['name']),
           c.record('kernels', k['name']), kernels.root(k)], timeout=600)


def install(m, k, observed):
    if m.get('installer'):
        hook(m, k, 'install')
        return
    if not re.search(r'(?m)^ID=(?:"?)(ubuntu|debian)(?:"?)$', observed['os_release']):
        c.fail('No built-in installer for this OS. Configure INSTALLER when registering the machine.')
    release = k['release']
    if observed['release'] == release and machines.match(k, observed) != 'verified':
        c.fail('Different/unknown builds share a release name. Use a unique LOCALVERSION or an installer that preserves both builds.')
    existing = c.ssh(m, 'if [ -f /boot/vmlinuz-' + shlex.quote(release) + ' ]; then sha256sum /boot/vmlinuz-' + shlex.quote(release) + '; fi', sudo=True).stdout.decode().split()
    if existing and existing[0] != c.digest(kernels.path(k, 'image')):
        c.fail('Boot image name collision. Refusing to overwrite an existing kernel; use a unique release or custom installer.')
    if not re.fullmatch(r'[A-Za-z0-9._+-]+', release):
        c.fail('Unsafe kernel release string.')
    remote = '/tmp/mapple-install-' + k['id']
    c.ssh(m, 'mkdir -p ' + shlex.quote(remote))
    for key in ('image', 'modules'):
        c.send(m, kernels.path(k, key), remote + '/' + key)
    build = kernels.path(k, 'build')
    if build and (build / '.config').exists():
        c.send(m, build / '.config', remote + '/config')
    system_map = kernels.path(k, 'system_map')
    if system_map:
        c.send(m, system_map, remote + '/System.map')
    # Extract into /lib/modules, never over /lib (which is a merged-/usr symlink).
    script = f'''set -eu
apt-get -qq update
DEBIAN_FRONTEND=noninteractive apt-get -qq install -y initramfs-tools grub-common cryptsetup-initramfs
install -m 644 {remote}/image /boot/vmlinuz-{release}
[ ! -f {remote}/config ] || install -m 644 {remote}/config /boot/config-{release}
[ ! -f {remote}/System.map ] || install -m 644 {remote}/System.map /boot/System.map-{release}
mkdir -p /lib/modules
tar --no-same-owner -xzf {remote}/modules -C /lib/modules
depmod -a {release}
if [ -f /boot/initrd.img-{release} ]; then update-initramfs -u -k {release}; else update-initramfs -c -k {release}; fi
mkdir -p /etc/default/grub.d
printf '%s\\n' 'GRUB_DEFAULT=saved' 'GRUB_SAVEDEFAULT=false' 'GRUB_TIMEOUT=3' 'GRUB_CMDLINE_LINUX_DEFAULT="$GRUB_CMDLINE_LINUX_DEFAULT nokaslr panic=10"' > /etc/default/grub.d/99-mapple.cfg
update-grub
'''
    if m.get('managed'):
        script += "mkdir -p /etc/cloud/cloud.cfg.d; printf 'manual_cache_clean: true\\n' > /etc/cloud/cloud.cfg.d/99-mapple-cache.cfg\n"
    c.ssh(m, script, sudo=True, timeout=600)
    text = c.ssh(m, 'cat /boot/grub/grub.cfg', sudo=True).stdout.decode()
    previous = grub_entry(text, observed['release'])
    candidate = grub_entry(text, release)
    c.ssh(m, 'grub-set-default ' + shlex.quote(previous) + '; grub-reboot ' + shlex.quote(candidate), sudo=True)
    m['transition']['grub_candidate'] = candidate
    m['transition']['grub_fallback'] = previous
    c.save(machines.slot_path(m['name']), m)


def prepare(m, k):
    with c.lock('machine-' + m['name']), c.lock(c.connection_key(m)), c.lock('kernel-' + k['name']):
        kernels.validate(k)
        capabilities = kernels.characteristics(k)
        observed = machines.inspect(m)
        if k['arch'] != observed['arch']:
            c.fail(f'Architecture mismatch: machine {observed["arch"]}, kernel {k["arch"]}.')
        if machines.match(k, observed) == 'verified' and 'nokaslr' in observed['cmdline'].split():
            m['kernel'], m['match'] = k['name'], 'verified'
            m.pop('requested_kernel', None)
            if m.get('transition', {}).get('to') == k['name'] and m['transition']['status'] != 'complete':
                m['transition'].update(status='complete', recovered=True, finished_at=c.now())
            c.save(machines.slot_path(m['name']), m)
            from . import runs
            runs.publish_navigation(m)
            print('Already running the verified kernel; no reboot needed.')
            return
        if not capabilities.get('build_id'):
            c.fail('Installation requires the expected GNU build ID (debug ELF or BUILD_ID on import).')
        if not capabilities['installable'] and not m.get('installer'):
            c.fail('Kernel has no installation image/module archive. Its debug tier does not imply installability.')
        m['transition'] = {'from': m.get('kernel'), 'to': k['name'], 'status': 'installing', 'started_at': c.now()}
        c.save(machines.slot_path(m['name']), m)
        try:
            install(m, k, observed)
            machines.invalidate_runs(m['name'])
            m['transition']['status'] = 'rebooting'
            c.save(machines.slot_path(m['name']), m)
            try:
                c.ssh(m, 'nohup sh -c "sleep 2; reboot" >/dev/null 2>&1 </dev/null &', sudo=True)
            except subprocess.CalledProcessError as error:
                if error.returncode != 255:
                    raise
            print('Waiting for the new boot…', flush=True)
            c.wait_ssh(m, old_boot=observed['boot_id'])
            current = machines.inspect(m)
            status = machines.match(k, current)
            if status != 'verified':
                c.fail('Boot verification failed: ' + status + '. Previous assignment retained.')
            if m.get('installer'):
                hook(m, k, 'commit')
            else:
                c.ssh(m, 'grub-set-default ' + shlex.quote(m['transition']['grub_candidate']), sudo=True)
            m.update(kernel=k['name'], match=status)
            m.pop('requested_kernel', None)
            m['transition'].update(status='complete', finished_at=c.now())
            c.save(machines.slot_path(m['name']), m)
            from . import runs
            runs.publish_navigation(m)
            print('Verified and assigned kernel ' + k['name'] + ' to ' + m['name'])
        except BaseException as error:
            m['transition'].update(status='failed', error=str(error), finished_at=c.now())
            c.save(machines.slot_path(m['name']), m)
            # One-shot GRUB keeps the old default. A subsequent boot will select
            # it; only a reachable machine/console can confirm recovery occurred.
            print('Preparation failed. Previous assignment and kernel retained; fallback recovery is not yet confirmed.')
            raise
