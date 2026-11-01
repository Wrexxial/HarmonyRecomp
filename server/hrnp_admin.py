#!/usr/bin/env python3
"""HarmonyRecomp online server - admin tool for the server owner.

Runs on your own PC (Windows, Linux, macOS) and talks to the server over SSH
(Windows 10/11 have ssh built in). Double-click HRNP-Admin.bat for a menu, or:

    python hrnp_admin.py status            who is online now + summary of 24 h
    python hrnp_admin.py log [--hours 48]  save the full log as a text file
    python hrnp_admin.py live              follow the log (Ctrl+C ends)
    python hrnp_admin.py update            upload hrnp_server.py and restart
    python hrnp_admin.py key               log in without a password from now on
    python hrnp_admin.py restart

Settings (server, SSH user) are asked once and kept in hrnp_admin.json next to
this file. The log contains player names and IP addresses: keep the saved
files to yourself.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import shutil
import subprocess
import sys
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
CONFIG = HERE / "hrnp_admin.json"
SERVICE = "hrnp"
REMOTE_DIR = "/opt/hrnp"
DEFAULTS = {"host": "87.106.10.23", "user": "root", "port": 22}

# Runs commands as root: directly when logged in as root, else with sudo.
ROOT = 'if [ "$(id -u)" = 0 ]; then S=; else S=sudo; fi; '


# ---- settings and ssh ------------------------------------------------------

def load_config(ask: bool = True) -> dict:
    config = dict(DEFAULTS)
    if CONFIG.exists():
        try:
            config.update(json.loads(CONFIG.read_text(encoding="utf-8")))
            return config
        except (OSError, ValueError):
            pass
    if ask:
        print("First start: where is the server?")
        for key, label in (("host", "Server address"), ("user", "SSH user")):
            answer = input(f"  {label} [{config[key]}]: ").strip()
            if answer:
                config[key] = answer
        CONFIG.write_text(json.dumps(config, indent=2), encoding="utf-8")
        print(f"  saved in {CONFIG.name}\n")
    return config


def target(config: dict) -> list[str]:
    return ["-p", str(config["port"]), f"{config['user']}@{config['host']}"]


def need(tool: str) -> str:
    path = shutil.which(tool)
    if not path:
        sys.exit(f"'{tool}' was not found. On Windows: Settings > System > Optional features > "
                 f"add 'OpenSSH Client'.")
    return path


def ssh(config: dict, command: str, capture: bool = False, tty: bool = False) -> str:
    args = [need("ssh"), "-o", "StrictHostKeyChecking=accept-new"]
    if tty:
        args.append("-t")
    args += target(config) + [command]
    if capture:
        result = subprocess.run(args, stdout=subprocess.PIPE, text=True, encoding="utf-8",
                                errors="replace")
        if result.returncode != 0 and not result.stdout:
            sys.exit(f"ssh failed (exit code {result.returncode}). Check the address, user and "
                     f"password (settings: {CONFIG.name}).")
        return result.stdout
    try:
        subprocess.run(args)
    except KeyboardInterrupt:
        pass
    return ""


def journal(config: dict, since: str, fresh_stats: bool = False) -> list[str]:
    command = ROOT
    if fresh_stats:
        # Ask the server for a "stats:" line right now (hrnp_server.py, SIGUSR1).
        command += f"$S systemctl kill -s USR1 {SERVICE} 2>/dev/null; sleep 2; "
    command += (f"$S journalctl -u {SERVICE} --since '{since}' -o short-iso --no-pager; "
                f"echo '##active '$(systemctl is-active {SERVICE})")
    return ssh(config, command, capture=True).splitlines()


# ---- reading the log -------------------------------------------------------

RE_CLIENT = re.compile(r"INFO client (\S+) (.*) from \(.*\), game (\S*)$")
RE_GONE = re.compile(r"INFO client \S+ (.*) \(.*\) gone: (said bye|timed out)")
RE_ROOM = re.compile(r"INFO room (\S+) opened by (.*) \((public|private), \d+/\d+\)$")
RE_CLOSED = re.compile(r"INFO room \S+ closed.* after (\d+) min, (\d+) joined, (relay used|no relay)")
RE_JOIN = re.compile(r"INFO (.*) connected to room (\S+) \(\d+ members\)$")
RE_STATS = re.compile(r"INFO stats: (\d+) clients \(peak (\d+)\), (\d+) rooms \((\d+) public\), "
                      r"versions (.*), relay (\d+) players (\d+) packets (\d+) KB")
RE_START = re.compile(r"INFO HarmonyRecomp online server on UDP")
RE_PROBLEM = re.compile(r"Traceback| ERROR |Error: |Main process exited|Failed with result")


def summarize(lines: list[str], hours: float) -> str:
    players: Counter[str] = Counter()
    versions: Counter[str] = Counter()
    rooms = public = joins = relay_rooms = timeouts = byes = starts = 0
    room_minutes: list[int] = []
    last_stats = None
    peak = 0
    relay_kb = 0
    problems: list[str] = []
    active = "?"
    for line in lines:
        if line.startswith("##active "):
            active = line.split(" ", 1)[1].strip()
        elif m := RE_STATS.search(line):
            last_stats = (line[:19], m)
            peak = max(peak, int(m.group(2)))
            relay_kb += int(m.group(8))
        elif m := RE_CLIENT.search(line):
            players[m.group(2)] += 1
            versions[m.group(3) or "?"] += 1
        elif m := RE_GONE.search(line):
            if m.group(2) == "timed out":
                timeouts += 1
            else:
                byes += 1
        elif m := RE_ROOM.search(line):
            rooms += 1
            public += m.group(3) == "public"
        elif m := RE_CLOSED.search(line):
            room_minutes.append(int(m.group(1)))
            relay_rooms += m.group(3) == "relay used"
        elif RE_JOIN.search(line):
            joins += 1
        elif RE_START.search(line):
            starts += 1
        elif RE_PROBLEM.search(line):
            problems.append(line)

    out = []
    state = {"active": "running", "inactive": "STOPPED", "failed": "CRASHED"}.get(active, active)
    out.append(f"Server: {state}")
    if last_stats:
        when, m = last_stats
        out.append(f"Online now: {m.group(1)} player(s), {m.group(3)} room(s) "
                   f"({m.group(4)} public), versions {m.group(5)}   [{when.replace('T', ' ')}]")
        if int(m.group(6)):
            out.append(f"            {m.group(6)} player(s) used the relay in the last minutes")
    else:
        out.append("Online now: unknown (the server is older than this tool: run 'update')")
    out.append("")
    out.append(f"Last {hours:g} hours:")
    out.append(f"  players            {len(players)} different, {sum(players.values())} connection(s)")
    if peak:
        out.append(f"  most at once       {peak}")
    if versions:
        out.append("  game versions      " + ", ".join(f"{v}: {n}" for v, n in versions.most_common()))
    out.append(f"  rooms opened       {rooms} ({public} public, {rooms - public} with code only)")
    out.append(f"  joins              {joins}")
    if room_minutes:
        avg = sum(room_minutes) / len(room_minutes)
        out.append(f"  rooms closed       {len(room_minutes)}, open {avg:.0f} min on average")
        out.append(f"  needed the relay   {relay_rooms} of {len(room_minutes)} room(s) "
                   f"(the rest played directly)")
    if relay_kb:
        out.append(f"  relay traffic      {relay_kb / 1024:.1f} MB")
    out.append(f"  left normally      {byes}, connection lost (timeout) {timeouts}")
    if starts:
        out.append(f"  server (re)starts  {starts}")
    if players:
        top = ", ".join(f"{name} ({n})" for name, n in players.most_common(10))
        out.append(f"  most active        {top}")
    if problems:
        out.append("")
        out.append(f"Problems ({len(problems)}), newest last:")
        out += ["  " + p for p in problems[-10:]]
    return "\n".join(out)


def out_dir() -> Path:
    # <...>/HarmonyRecomp/server -> <...>/Issues when that folder exists.
    for candidate in (HERE.parent.parent / "Issues", HERE.parent.parent / "issues"):
        if candidate.is_dir():
            return candidate
    return Path.cwd()


def save_log(lines: list[str], hours: float, summary: str) -> Path:
    stamp = dt.datetime.now().strftime("%Y-%m-%d_%H%M")
    path = out_dir() / f"server-log_{stamp}.txt"
    body = [l for l in lines if not l.startswith("##")]
    path.write_text(f"HarmonyRecomp server log, last {hours:g} hours\n\n{summary}\n\n"
                    f"{'=' * 70}\n" + "\n".join(body) + "\n", encoding="utf-8")
    return path


# ---- commands --------------------------------------------------------------

def cmd_status(config: dict, hours: float, save: bool) -> None:
    lines = journal(config, f"{hours:g} hours ago", fresh_stats=True)
    summary = summarize(lines, hours)
    print(summary)
    if save:
        print(f"\nFull log saved: {save_log(lines, hours, summary)}")


def cmd_log(config: dict, hours: float) -> None:
    lines = journal(config, f"{hours:g} hours ago")
    summary = summarize(lines, hours)
    print(summary)
    print(f"\nFull log saved: {save_log(lines, hours, summary)}")
    print("(contains player names and IP addresses - keep it to yourself)")


def cmd_live(config: dict) -> None:
    print("Live log, Ctrl+C ends.\n")
    ssh(config, ROOT + f"$S journalctl -u {SERVICE} -f -n 30 -o cat", tty=True)


def online_now(config: dict) -> int | None:
    lines = journal(config, "10 minutes ago", fresh_stats=True)
    for line in reversed(lines):
        if m := RE_STATS.search(line):
            return int(m.group(1))
    return None


def cmd_restart(config: dict, confirm: bool = True) -> None:
    if confirm and not ask_if_busy(config, "restart"):
        return
    ssh(config, ROOT + f"$S systemctl restart {SERVICE} && systemctl is-active {SERVICE}")


def ask_if_busy(config: dict, what: str) -> bool:
    count = online_now(config)
    if count:
        answer = input(f"{count} player(s) are online right now; a {what} drops their games. "
                       f"Continue? [y/N] ").strip().lower()
        return answer in ("y", "yes", "j", "ja")
    return True


def cmd_update(config: dict) -> None:
    source = HERE / "hrnp_server.py"
    if not source.exists():
        sys.exit(f"{source} not found")
    if not ask_if_busy(config, "server update"):
        return
    print("Uploading hrnp_server.py ...")
    scp = [need("scp"), "-o", "StrictHostKeyChecking=accept-new", "-P", str(config["port"]),
           str(source), f"{config['user']}@{config['host']}:/tmp/hrnp_server.py"]
    if subprocess.run(scp).returncode != 0:
        sys.exit("upload failed")
    ssh(config, ROOT + f"$S install -m 644 /tmp/hrnp_server.py {REMOTE_DIR}/hrnp_server.py && "
                       f"rm -f /tmp/hrnp_server.py && $S systemctl restart {SERVICE} && "
                       f"sleep 1 && echo \"server: $(systemctl is-active {SERVICE})\"")


def cmd_key(config: dict) -> None:
    key = Path.home() / ".ssh" / "id_ed25519"
    if not key.exists():
        print("Creating an SSH key on this PC (just press Enter at the questions) ...")
        key.parent.mkdir(exist_ok=True)
        subprocess.run([need("ssh-keygen"), "-t", "ed25519", "-f", str(key)])
    public = key.with_suffix(".pub").read_text(encoding="utf-8").strip()
    print("Copying it to the server (enter the server password one last time) ...")
    ssh(config, "mkdir -p ~/.ssh && chmod 700 ~/.ssh && "
                f"grep -qxF '{public}' ~/.ssh/authorized_keys 2>/dev/null || "
                f"echo '{public}' >> ~/.ssh/authorized_keys; chmod 600 ~/.ssh/authorized_keys; "
                "echo done")


def menu(config: dict) -> None:
    items = [
        ("Status (online now + last 24 hours)", lambda: cmd_status(config, 24, False)),
        ("Status + save the log for Claude/issues (24 h)", lambda: cmd_log(config, 24)),
        ("Save the log of the last 7 days", lambda: cmd_log(config, 24 * 7)),
        ("Live log", lambda: cmd_live(config)),
        ("Update the server (upload hrnp_server.py, restart)", lambda: cmd_update(config)),
        ("Restart the server", lambda: cmd_restart(config)),
        ("Log in without password from now on (SSH key)", lambda: cmd_key(config)),
    ]
    while True:
        print(f"\nHarmonyRecomp server {config['user']}@{config['host']}")
        for i, (label, _) in enumerate(items, 1):
            print(f"  {i}  {label}")
        print("  0  Exit")
        choice = input("> ").strip()
        if choice in ("0", "", "q"):
            return
        if choice.isdigit() and 1 <= int(choice) <= len(items):
            print()
            items[int(choice) - 1][1]()
        else:
            print("?")


def main() -> None:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", nargs="?",
                        choices=["status", "log", "live", "update", "restart", "key", "menu"],
                        default="menu")
    parser.add_argument("--hours", type=float, default=24)
    parser.add_argument("--save", action="store_true", help="status: also save the log")
    parser.add_argument("--file", help="summarize a saved log file instead of asking the server")
    args = parser.parse_args()

    if args.file:
        lines = Path(args.file).read_text(encoding="utf-8", errors="replace").splitlines()
        print(summarize(lines, args.hours))
        return
    config = load_config()
    try:
        {
            "status": lambda: cmd_status(config, args.hours, args.save),
            "log": lambda: cmd_log(config, args.hours),
            "live": lambda: cmd_live(config),
            "update": lambda: cmd_update(config),
            "restart": lambda: cmd_restart(config),
            "key": lambda: cmd_key(config),
            "menu": lambda: menu(config),
        }[args.command]()
    except KeyboardInterrupt:
        print()


if __name__ == "__main__":
    main()
