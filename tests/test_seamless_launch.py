#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
import importlib.util
import json
import os
from pathlib import Path
import stat
import tempfile
import time
import unittest
spec=importlib.util.spec_from_file_location('seamless',Path(__file__).resolve().parents[1]/'scripts/seamless.py')
s=importlib.util.module_from_spec(spec);spec.loader.exec_module(s)
class SeamlessLaunch(unittest.TestCase):
    def global_env(self, account='u0123456789abcd'):
        return {'BB_SEAMLESS_HOST':'global','BB_SHADNET_NPID':account,
                'BB_SHADNET_PASSWORD':f'{int(time.time()):08x}'+'a'*24}
    def test_global_requires_signin_without_creating_automatic_account(self):
        with tempfile.TemporaryDirectory() as temp:
            data=Path(temp)
            with self.assertRaisesRegex(ValueError,'Sign in'):
                s.launch_environment({'BB_SEAMLESS_HOST':'global'},data)
            self.assertFalse((data/'seamless/identities').exists())
    def test_global_accounts_have_separate_saves_and_pinned_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            data=Path(temp);(data/'bbport.ini').write_text('seamless_host_rules=invalid-local-rule\nseamless_harder_enemies=bad\n')
            first=s.launch_environment(self.global_env(),data)
            second=s.launch_environment(self.global_env('uabcdef01234567'),data)
            self.assertNotEqual(first['BB_USER_DIR'],second['BB_USER_DIR'])
            self.assertEqual(first['BB_SHADNET_SERVER'],s.GLOBAL_HOST+':31314')
            self.assertEqual(first['BB_SHADNET_SERVER_KEY'],s.GLOBAL_PIN)
            self.assertEqual(first['BB_RELEASE_VERSION'],'1.4.0')
            self.assertNotIn('SHADPS4_BLOODBORNE_HOST_RULES',first)
            self.assertNotIn('SHADPS4_BLOODBORNE_HARDER_ENEMIES',first)
            for path in data.rglob('*'):
                if path.is_file():self.assertNotIn(first['BB_SHADNET_PASSWORD'],path.read_text())
    def test_global_expired_or_mismatched_sessions_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            env=self.global_env();env['BB_SHADNET_PASSWORD']='00000000'+'a'*24
            with self.assertRaisesRegex(ValueError,'expired'):s.launch_environment(env,Path(temp))
            env=self.global_env();env['BB_SEAMLESS_GLOBAL_ID']='uabcdef01234567'
            with self.assertRaisesRegex(ValueError,'differ'):s.launch_environment(env,Path(temp))
    def test_public_host_typed_directly_still_requires_signin(self):
        self.assertEqual(s.server('https://seamlesshunt.com'),('seamlesshunt.com',31314))
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(ValueError,'Sign in'):
                s.launch_environment({'BB_SHADNET_SERVER':'seamlesshunt.com'},Path(temp))
    def test_global_reads_only_its_targeted_keychain_item(self):
        with tempfile.TemporaryDirectory() as temp:
            data=Path(temp);env=self.global_env();secret=env.pop('BB_SHADNET_PASSWORD')
            account=env.pop('BB_SHADNET_NPID');env['BB_SEAMLESS_GLOBAL_ID']=account
            tool=data/'keychain';tool.write_text('#!/bin/sh\n[ "$1" = find-generic-password ] && [ "$3" = bbport-seamless-global ] && [ "$5" = seamlesshunt.com:'+account+' ] && [ "$6" = -w ] || exit 2\nprintf %s '+secret+'\n');tool.chmod(0o700)
            env['BB_KEYCHAIN_TOOL']=str(tool)
            prepared=s.launch_environment(env,data)
            self.assertEqual(prepared['BB_SHADNET_PASSWORD'],secret)
    def test_v140_phantom_glow_setting_reaches_client(self):
        with tempfile.TemporaryDirectory() as temp:
            data=Path(temp);(data/'bbport.ini').write_text('seamless_phantom_glow=0\n')
            self.assertEqual(s.launch_environment({},data)['SHADPS4_BLOODBORNE_NO_PHANTOM_GLOW'],'1')
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
    def test_v131_boss_tempo_rules_reach_client(self):
        with tempfile.TemporaryDirectory() as temp:
            data=Path(temp)
            for tempo in ('aggressive','relentless'):
                (data/'bbport.ini').write_text('seamless_host_rules=travel,vote,'+tempo+'\n')
                env=s.launch_environment({},data)
                self.assertEqual(env['SHADPS4_BLOODBORNE_HOST_RULES'],'travel,vote,'+tempo)
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
