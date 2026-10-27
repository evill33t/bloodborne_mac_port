#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
import importlib.util
import json
import os
from pathlib import Path
import stat
import tempfile
import unittest
spec=importlib.util.spec_from_file_location('seamless',Path(__file__).resolve().parents[1]/'scripts/seamless.py')
s=importlib.util.module_from_spec(spec);spec.loader.exec_module(s)
class SeamlessLaunch(unittest.TestCase):
    def test_identity_persists_and_servers_are_separate(self):
        with tempfile.TemporaryDirectory() as temp:
            folder=Path(temp)
            a=s.identity(folder,'one.example:31314');b=s.identity(folder,'one.example:31314')
            self.assertEqual(a,b)
            self.assertNotEqual(a['account'],s.identity(folder,'two.example:31314')['account'])
            self.assertLessEqual(len(a['account']),16)
            self.assertTrue(all(stat.S_IMODE(p.stat().st_mode)==0o600 for p in folder.iterdir()))
    def test_separate_saves_and_default_routes(self):
        with tempfile.TemporaryDirectory() as temp:
            data=Path(temp);original=data/'user/savedata/slot';original.parent.mkdir(parents=True);original.write_bytes(b'original save')
            env=s.launch_environment({},data)
            user=Path(env['BB_USER_DIR']);self.assertNotEqual(user,data/'user')
            self.assertEqual((user/'savedata/slot').read_bytes(),original.read_bytes())
            (user/'savedata/slot').write_bytes(b'co-op save');self.assertEqual(original.read_bytes(),b'original save')
            again=s.launch_environment({},data);self.assertEqual(again['BB_SHADNET_NPID'],env['BB_SHADNET_NPID'])
            self.assertEqual((user/'savedata/slot').read_bytes(),b'co-op save')
            self.assertEqual(env['BB_SHADNET_SERVER'],s.HOST+':31314');self.assertEqual(env['BB_SHADNET_SERVER_KEY'],s.PIN)
            self.assertEqual(env['BB_FPS'],'60');self.assertEqual(env['BB_UPNP'],'0')
            routes=json.loads(Path(env['SHADPS4_HTTP_HOST_OVERRIDES_JSON']).read_text())
            self.assertEqual(routes['https://ss4.scej-network.jp:20443'],'http://'+s.HOST+':20443')
    def test_overrides_and_no_secret_mixing(self):
        with tempfile.TemporaryDirectory() as temp:
            data=Path(temp);(data/'bbport.ini').write_text('seamless_host_rules=travel,vote,scale\nseamless_harder_enemies=3,1.5,3\n')
            env=s.launch_environment({'BB_SHADNET_SERVER':'other.example:32000','BB_USER_DIR':str(data/'explicit'),'BB_P2P_PORT':'3658'},data)
            self.assertNotIn('SHADPS4_P2P_PORT',env)
            self.assertNotIn('BB_SHADNET_SERVER_KEY',env);self.assertEqual(env['BB_SHADNET_WEBAPI'],'http://other.example:31315')
            self.assertEqual(env['BB_USER_DIR'],str(data/'explicit'))
            self.assertEqual(env['SHADPS4_BLOODBORNE_HOST_RULES'],'travel,vote,scale')
            with self.assertRaises(ValueError): s.launch_environment({'BB_SHADNET_NPID':'half'},data)
    def test_launcher_custom_save_folder_is_copied_then_isolated(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);selected=root/'chosen saves';selected.mkdir();(selected/'slot').write_bytes(b'original')
            data=root/'data'
            env=s.launch_environment({'BB_SAVE_DIR':str(selected)},data)
            actual=Path(env['BB_SAVE_DIR'])
            self.assertEqual(actual,Path(env['BB_USER_DIR'])/'savedata')
            self.assertNotEqual(actual,selected)
            self.assertEqual((actual/'slot').read_bytes(),b'original')
            (actual/'slot').write_bytes(b'co-op')
            self.assertEqual((selected/'slot').read_bytes(),b'original')
            again=s.launch_environment({'BB_SAVE_DIR':str(selected)},data)
            self.assertEqual(Path(again['BB_SAVE_DIR']),actual)
            self.assertEqual((actual/'slot').read_bytes(),b'co-op')
    def test_bad_address_and_unsafe_identity_fail(self):
        for address in ('https://x.example','name:0','name:65536','user@host:31314','host:123/path'):
            with self.assertRaises(ValueError): s.server(address)
        with tempfile.TemporaryDirectory() as temp:
            folder=Path(temp);s.identity(folder,'test:31314');path=next(folder.iterdir());path.chmod(0o644)
            with self.assertRaises(ValueError): s.identity(folder,'test:31314')
if __name__=='__main__': unittest.main()
