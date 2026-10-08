#!/usr/bin/env python3
"""A WebUser entry for WebAuth, with the password hashed (PBKDF2-HMAC-SHA256).

    python3 web_user.py ann operator              asks for the password
    python3 web_user.py ann operator --iterations 20000

Prints a line to paste into the firmware's table of users:

    {"ann", WebRole::Operator, 20000, {0x.., ...}, {0x.., ...}},

The password itself is never stored. Roles: viewer, operator, admin.
More iterations make a stolen table slower to attack, and each login
slower on the device (about a second for 20000 on a 120 MHz Cortex-M3,
by estimate): pick what the device can afford.
"""
import argparse
import getpass
import hashlib
import os
import sys

ROLES = {"viewer": "Viewer", "operator": "Operator", "admin": "Admin"}


def entry(name, role, password, iterations, salt=None):
    salt = salt if salt is not None else os.urandom(16)
    digest = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, iterations, 32)
    hexes = lambda b: ", ".join("0x%02x" % x for x in b)
    return '{"%s", WebRole::%s, %d, {%s}, {%s}},' % (name, ROLES[role], iterations, hexes(salt), hexes(digest))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("name")
    p.add_argument("role", choices=sorted(ROLES))
    p.add_argument("--iterations", type=int, default=20000)
    p.add_argument("--password", help="for scripts; otherwise asked for (not echoed)")
    a = p.parse_args()
    if len(a.name) > 31 or not a.name or any(c in a.name for c in '"\\'):
        sys.exit("a name of 1 to 31 characters, without quotes or backslashes")
    pw = a.password
    if pw is None:
        pw = getpass.getpass("password: ")
        if pw != getpass.getpass("again: "):
            sys.exit("they differ")
    if len(pw) < 8 or len(pw) > 127:
        sys.exit("a password of 8 to 127 characters")
    print(entry(a.name, a.role, pw, a.iterations))


if __name__ == "__main__":
    main()
