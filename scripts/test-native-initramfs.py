#!/usr/bin/env python3
"""Exercise the native initramfs builder with stand-in module bundles."""
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RELEASE_PREFIX = '7.2.11-aurora-hmos-j514s-test'
BASE_MODULES = '''
    pinctrl-apple-gpio gpio-shared-proxy spi-apple i2c-pasemi-platform
    spmi-apple-controller apple_nvmem_spmi macsmc macsmc-power gpio-macsmc
    sn201202x tps6598x phy-apple-atc dwc3-apple thunderbolt_apple onboard_usb_dev
    mux-apple-display-crossbar phy-apple-dptx typec_displayport apple-sart
    nvme-apple apple-rtkit-helper apple-sio apple-dockchannel dockchannel-hid
    spi-hid-apple-of hid-apple hid-magicmouse hid-multitouch macsmc-input
    apple-pmp appledrm
'''.split()


class NativeInitramfsTests(unittest.TestCase):
    def build(self, folder, release, additional, *options):
        modules = folder / 'modules'
        module_tree = modules / 'lib/modules' / release
        (module_tree / 'kernel').mkdir(parents=True)
        entries = {name: [] for name in BASE_MODULES}
        entries.update(additional)
        dependencies = []
        for name, requires in entries.items():
            path = f'kernel/{name}.ko'
            (module_tree / path).write_bytes(release.encode() + b' module fixture')
            dependencies.append(path + ': ' + ' '.join(f'kernel/{dep}.ko' for dep in requires))
        (module_tree / 'modules.dep').write_text('\n'.join(dependencies) + '\n')
        for name in ('modules.builtin', 'modules.builtin.modinfo', 'modules.softdep'):
            (module_tree / name).touch()
        (module_tree / 'modules.alias').write_text('alias pci:v000014E4d00004433* brcmfmac\n')
        bootstrap = folder / 'bootstrap'
        (bootstrap / 'bin').mkdir(parents=True)
        for name in ('init', 'bin/busybox', 'bin/binder-setup', 'bin/input-device-permissions'):
            (bootstrap / name).write_bytes(b'\x7fELF')
        (bootstrap / 'bin/hmos_ashmem.ko').write_bytes(release.encode() + b' module fixture')
        firmware = folder / 'firmware.tar'
        with tarfile.open(firmware, 'w'):
            pass
        helper = bootstrap / 'init'
        output = folder / 'output'
        result = subprocess.run([
            sys.executable, str(ROOT / 'ports/tools/build-native-initramfs.py'),
            '--modules', str(modules), '--bootstrap', str(bootstrap), '--firmware', str(firmware),
            '--ext4-uuid', str(helper), '--kernel-log', str(helper), '--fat-uuid', str(helper),
            '--output', str(output), '--release', release, *options,
        ], capture_output=True, text=True)
        return result, output / 'root'

    def test_default_build_accepts_older_bundles(self):
        for version in ('7', '8'):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as folder:
                release = RELEASE_PREFIX + version
                result, root = self.build(Path(folder), release, {
                    'brcmfmac': [],
                    'brcmfmac-wcc': ['brcmfmac'],
                })
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertNotIn('macsmc-reboot', (root / 'aurora-init').read_text())
                self.assertFalse((root / f'lib/modules/{release}/kernel/brcmfmac.ko').exists())

    def test_reboot_module_and_kernel_wifi_request(self):
        with tempfile.TemporaryDirectory() as folder:
            release = RELEASE_PREFIX + '7'
            result, root = self.build(Path(folder), release, {
                'macsmc-reboot': ['macsmc'],
                'brcmfmac': [],
                'brcmfmac-wcc': ['brcmfmac'],
            }, '--early-wcc-wifi')
            self.assertEqual(result.returncode, 0, result.stderr)
            init = (root / 'aurora-init').read_text()
            self.assertLess(init.index('macsmc macsmc-reboot macsmc-power'), init.index('for alias in'))
            self.assertEqual((root / 'sbin/modprobe').readlink().as_posix(), '../bin/modprobe')
            loader = (root / 'sbin/modprobe').read_text()
            # Exercise the generated matching and ordering without loading host modules.
            loader = loader.replace('/bin/grep', 'false').replace('/bin/insmod', 'echo')
            for args, expected in [
                (('macsmc-reboot',), ['macsmc', 'macsmc-reboot']),
                (('pci:v000014E4d00004433sv0000',), ['brcmfmac']),
                (('-q', '--', 'brcmfmac-wcc'), ['brcmfmac', 'brcmfmac-wcc']),
            ]:
                with self.subTest(args=args):
                    loaded = subprocess.run(['sh', '-c', loader, 'modprobe', *args],
                                            capture_output=True, text=True)
                    self.assertEqual(loaded.returncode, 0, loaded.stderr)
                    actual = [Path(line).name for line in loaded.stdout.splitlines()]
                    self.assertEqual(actual, [f'{name}.ko' for name in expected])
                    for name in actual:
                        self.assertTrue((root / f'lib/modules/{release}/kernel/{name}').is_file())

    def test_early_wifi_requires_vendor_module(self):
        with tempfile.TemporaryDirectory() as folder:
            result, root = self.build(Path(folder), RELEASE_PREFIX + '7',
                                      {'brcmfmac': []}, '--early-wcc-wifi')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Missing module requirements: ['brcmfmac-wcc']", result.stderr)
            self.assertFalse(root.exists())


if __name__ == '__main__':
    unittest.main()
