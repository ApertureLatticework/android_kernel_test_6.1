#!/usr/bin/env python3
import re
import sys
import subprocess

def get_head_commit_message() -> str:
    result = subprocess.run(
        ["git", "show", "-s", "--format=%B", "HEAD"],
        capture_output=True,
        text=True
    )
    if result.returncode != 0:
        sys.exit(1)
    return result.stdout

def main():
    msg = get_head_commit_message()
    lines = [line.rstrip('\n') for line in msg.splitlines()]
    change_id_lines = [line for line in lines if line.startswith("Change-Id: ")]
    if not change_id_lines:
        print("Missing Change-Id line in commit message.")
        sys.exit(1)
    if len(change_id_lines) > 1:
        print("Multiple Change-Id lines found.")
        sys.exit(1)
    line = change_id_lines[0]
    if line != line.strip():
        print("Change-Id line contains leading or trailing whitespace.")
        sys.exit(1)
    if not re.fullmatch(r'Change-Id: I[0-9a-f]{40}', line):
        print("Invalid Change-Id format.")
        sys.exit(1)
    print("Valid Change-Id.")

if __name__ == "__main__":
    main()
