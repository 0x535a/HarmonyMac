#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Prepare a new offline settings DB copy for a development desktop candidate.

Only two local first-boot settings change. The input is never written, and this
tool neither mounts images nor installs its output into a root filesystem.
"""
import argparse
from contextlib import closing
import hashlib
import json
import os
from pathlib import Path
import sqlite3


def prepare(source, output, user_id=100, ota_version=None):
    source=Path(source).resolve(strict=True)
    output=Path(output).absolute()
    if not source.is_file() or not 0<=user_id<=999999:
        raise ValueError('Expected a database file and user ID 0..999999')
    if output.resolve()==source or output.exists() or output.is_symlink():
        raise ValueError('Output must be a new file distinct from the input')
    for suffix in ('-wal','-shm','-journal'):
        if Path(str(source)+suffix).exists():
            raise ValueError('Input must be an offline, checkpointed copy without SQLite sidecars')
    before_sha=hashlib.sha256(source.read_bytes()).hexdigest()
    memory=sqlite3.connect(':memory:')
    created=False
    try:
        # immutable avoids creating SHM/lock sidecars beside this offline input.
        with closing(sqlite3.connect(source.as_uri()+'?mode=ro&immutable=1',uri=True)) as original:
            original.backup(memory)
        if memory.execute('PRAGMA integrity_check').fetchall()!=[('ok',)]:
            raise ValueError('Input SQLite integrity check failed')
        changes=[]
        updates=[('SETTINGSDATA','device_provisioned','1'),
                 (f'USER_SETTINGSDATA_SECURE_{user_id}','user_setup_complete','1')]
        if ota_version:
            existing_tables={row[0] for row in memory.execute(
                "SELECT name FROM sqlite_master WHERE type='table'")}
            ota_tables=[t for t in ('SETTINGSDATA',f'USER_SETTINGSDATA_{user_id}',
                                    f'USER_SETTINGSDATA_SECURE_{user_id}') if t in existing_tables]
            for table in ota_tables:
                updates.extend([
                    (table,'settings.control.ota_old_version',ota_version),
                    (table,'buildversionrelease',ota_version),
                    (table,'is_ota_finished','1'),
                ])
        for table,key,value in updates:
            columns={row[1].upper() for row in memory.execute(f'PRAGMA table_info("{table}")')}
            if not {'KEYWORD','VALUE'}<=columns:
                raise ValueError(f'Unsupported schema: {table} requires KEYWORD and VALUE')
            rows=memory.execute(f'SELECT VALUE FROM "{table}" WHERE KEYWORD=?',(key,)).fetchall()
            if len(rows)>1:
                raise ValueError(f'Ambiguous duplicate setting: {key}')
            if rows:
                memory.execute(f'UPDATE "{table}" SET VALUE=? WHERE KEYWORD=?',(value,key))
            else:
                memory.execute(f'INSERT INTO "{table}" (KEYWORD,VALUE) VALUES (?,?)',(key,value))
            changes.append(dict(table=table,key=key,before=rows[0][0] if rows else None,after=value))
        memory.commit()
        if memory.execute('PRAGMA integrity_check').fetchall()!=[('ok',)]:
            raise ValueError('Prepared SQLite integrity check failed')
        if hashlib.sha256(source.read_bytes()).hexdigest()!=before_sha:
            raise ValueError('Input changed during preparation; stop its writer first')
        fd=os.open(output,os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o600)
        created=True
        os.close(fd)
        with closing(sqlite3.connect(output)) as destination:
            memory.backup(destination)
        return dict(source=source.name,source_sha256=before_sha,
                    output=output.name,output_sha256=hashlib.sha256(output.read_bytes()).hexdigest(),
                    changes=changes,scope='Local first-boot settings only; no commercial activation')
    except Exception:
        if created:output.unlink(missing_ok=True)
        raise
    finally:
        memory.close()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('source',type=Path)
    p.add_argument('output',type=Path)
    p.add_argument('--user-id',type=int,default=100)
    p.add_argument('--ota-version',default=None,
                   help='Optional displayVersion (e.g. "OpenHarmony 6.0.0.110") to align SCBOtaManager keys')
    a=p.parse_args()
    try:print(json.dumps(prepare(a.source,a.output,a.user_id,a.ota_version),indent=2))
    except (ValueError,OSError,sqlite3.Error) as error:p.exit(1,f'{error}\n')


if __name__=='__main__':
    main()
