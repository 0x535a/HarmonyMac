#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Offline fixtures only: no guest, mounted image, or live system is accessed."""
from pathlib import Path
import runpy
import sqlite3
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
prepare=runpy.run_path(str(ROOT/'ports/tools/prepare-oobe-settings.py'))['prepare']


class OobeTools(unittest.TestCase):
    def test_settings_copy_and_refusals(self):
        with tempfile.TemporaryDirectory() as folder:
            source=Path(folder)/'settingsdata.db'
            output=Path(folder)/'prepared.db'
            with sqlite3.connect(source) as db:
                for table in ['SETTINGSDATA','USER_SETTINGSDATA_SECURE_100']:
                    db.execute(f'CREATE TABLE {table} (KEYWORD TEXT PRIMARY KEY, VALUE TEXT)')
                db.execute('INSERT INTO SETTINGSDATA VALUES (?,?)',('device_provisioned','0'))
                db.execute('INSERT INTO SETTINGSDATA VALUES (?,?)',('unrelated','keep'))
            original=source.read_bytes()
            result=prepare(source,output)
            self.assertEqual(source.read_bytes(),original)
            self.assertEqual(len(result['changes']),2)
            with sqlite3.connect(output) as db:
                self.assertEqual(dict(db.execute('SELECT * FROM SETTINGSDATA')),
                                 dict(device_provisioned='1',unrelated='keep'))
                self.assertEqual(db.execute('SELECT * FROM USER_SETTINGSDATA_SECURE_100').fetchall(),
                                 [('user_setup_complete','1')])
                self.assertEqual(db.execute('PRAGMA integrity_check').fetchone(),('ok',))
            prepared=output.read_bytes()
            with self.assertRaises(ValueError):prepare(source,output)
            self.assertEqual(output.read_bytes(),prepared)
            with self.assertRaises(ValueError):prepare(source,source)
            with self.assertRaises(ValueError):prepare(source,Path(folder)/'wrong-user.db',101)
            self.assertFalse((Path(folder)/'wrong-user.db').exists())
            Path(str(source)+'-wal').write_bytes(b'pending journal')
            with self.assertRaises(ValueError):prepare(source,Path(folder)/'unsafe.db')
            self.assertFalse((Path(folder)/'unsafe.db').exists())

    def test_guest_dry_run(self):
        script=ROOT/'ports/tests/enter-development-desktop.sh'
        result=subprocess.run(['sh',str(script),'--wait','0','--settle','0','--dry-run'],
                              capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('/system/bin/aa force-stop com.huawei.hmos.hwstartupguide',result.stdout)
        invalid=subprocess.run(['sh',str(script),'--wait','invalid','--dry-run'],capture_output=True)
        self.assertEqual(invalid.returncode,2)


if __name__=='__main__':unittest.main()
