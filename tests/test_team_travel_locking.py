# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile the production travel state machine without a game image or network.

The receive callback runs under stream_mutex; sending from AnswerTeamTravel and
TickTeamTravel takes that same mutex with travel_mutex held. The fixture forces
receive to overlap a held state lock and checks that it completes independently.
The old shared lock fails in one second, without hanging the test runner.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


class TeamTravelLockingTests(unittest.TestCase):
    def test_receive_does_not_wait_for_travel_state(self):
        root = next(p for p in HERE.parents if
                    (p / 'emulator/src/core/bloodborne_host_rules_game.cpp').is_file() or
                    (p / 'gpu/bbseamless/vendor/core/bloodborne_host_rules_game.cpp').is_file())
        headers = root / 'emulator/src'
        if not headers.is_dir():
            headers = root / 'gpu/bbseamless/vendor'
        source = (headers / 'core/bloodborne_host_rules_game.cpp').read_text()
        # Keep the complete production travel implementation and real TravelVote. Only
        # memory/game queries, clock, logging, protobuf values and transport are adapters.
        marker = '// Kaniel 2026-10-25, the travel together rule:'
        self.assertIn(marker, source)
        implementation = source[source.index(marker):]
        fixture = (HERE / 'fixtures/team_travel.cpp.in').read_text().replace(
            '// TEAM_TRAVEL_IMPLEMENTATION', implementation)
        compiler = shlex.split(os.environ.get('CXX', 'c++'))
        if not compiler or not shutil.which(compiler[0]):
            self.skipTest('C++20 compiler is unavailable')
        with tempfile.TemporaryDirectory(prefix='team-travel-') as directory:
            cpp = Path(directory) / 'fixture.cpp'
            exe = Path(directory) / 'fixture'
            cpp.write_text(fixture)
            build = compiler + ['-std=c++20', '-pthread',
                                '-I' + str(root / 'gpu/bbseamless/include'),
                                '-I' + str(headers), '-I' + str(root / 'server'),
                                str(cpp), str(headers / 'core/bloodborne_host_rules.cpp'),
                                '-o', str(exe)]
            subprocess.run(build, check=True, capture_output=True, text=True, timeout=60)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS:', result.stdout)


if __name__ == '__main__':
    unittest.main()
