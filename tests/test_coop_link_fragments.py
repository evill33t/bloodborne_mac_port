# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the actual shared transport code and Mac capability encoding without a game."""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CoopLinkFragmentTests(unittest.TestCase):
    def test_transport_and_client_wire(self):
        compiler = shlex.split(os.environ.get('CXX', 'c++'))
        if not compiler or not shutil.which(compiler[0]):
            self.skipTest('C++20 compiler is unavailable')
        with tempfile.TemporaryDirectory(prefix='bb-link-fragments-') as directory:
            for name, source, includes in [
                ('server', ROOT / 'server/tests/test_link_fragments.cpp', [ROOT / 'server']),
                ('client', ROOT / 'server/tests/test_link_fragments.cpp', [ROOT / 'gpu/bbseamless/vendor']),
                ('wire', ROOT / 'tests/fixtures/coop_link_wire.cpp',
                 [ROOT / 'gpu/bbseamless/include', ROOT / 'gpu/bbseamless/vendor']),
            ]:
                binary = Path(directory) / name
                subprocess.run(compiler + ['-std=c++20', '-DNDEBUG', '-Wall', '-Wextra', '-Werror'] +
                               ['-I' + str(path) for path in includes] + [str(source), '-o', str(binary)],
                               check=True, capture_output=True, text=True, timeout=60)
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('PASS:', result.stdout)
        self.assertEqual((ROOT / 'server/coop/link_fragments.h').read_bytes(),
                         (ROOT / 'gpu/bbseamless/vendor/coop/link_fragments.h').read_bytes())


if __name__ == '__main__':
    unittest.main()
