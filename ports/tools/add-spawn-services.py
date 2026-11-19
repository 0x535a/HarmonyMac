#!/usr/bin/env python3
"""Restore stock nwebspawn, nativespawn and cjappspawn to an offline init.cfg."""
import argparse
import json
from pathlib import Path

SERVICES = ("nwebspawn", "nativespawn", "cjappspawn")
INIT_LIMIT = 100 * 1024


def restore(source, original_dir):
    config = json.loads(source.read_text(encoding="utf-8"))
    services = config["services"]
    names = {entry["name"]: entry for entry in services}
    appspawn = names["appspawn"]
    added = []
    for name in SERVICES:
        if name in names:
            continue
        stock_file = original_dir / f"{name}.cfg"
        stock = json.loads(stock_file.read_text(encoding="utf-8"))
        service = next((entry for entry in stock["services"] if entry["name"] == name), None)
        if service is None:
            raise ValueError(f"{stock_file} has no {name} service")
        service = dict(service, env=list(appspawn.get("env", [])), console=1)
        if name == "nwebspawn":
            # The stock service lacks an on-demand socket. Start it with appspawn.
            service.update({"start-mode": "condition", "disabled": 1})
        services.append(service)
        added.append(name)
    if "nwebspawn" in added:
        jobs = [job for job in config["jobs"] if "start appspawn" in job.get("cmds", [])]
        if len(jobs) != 1:
            raise ValueError("expected exactly one job that starts appspawn")
        commands = jobs[0]["cmds"]
        if "start nwebspawn" not in commands:
            commands.insert(commands.index("start appspawn") + 1, "start nwebspawn")
    data = (json.dumps(config, separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8")
    if len(data) >= INIT_LIMIT:
        raise ValueError(f"init.cfg would be {len(data)} bytes; expected fewer than {INIT_LIMIT}")
    return data, added


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("init_cfg", type=Path)
    parser.add_argument("original_init_dir", type=Path)
    parser.add_argument("output", type=Path, help="new file outside the running system")
    args = parser.parse_args()
    try:
        data, added = restore(args.init_cfg, args.original_init_dir)
        with args.output.open("xb") as output:
            output.write(data)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        parser.error(str(exc))
    print(json.dumps({"added": added, "bytes": len(data)}))


if __name__ == "__main__":
    main()
