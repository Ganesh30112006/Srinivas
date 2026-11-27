"""Non-interactive first-run data for a new server: an administrator, subjects and recorders.

  ADMIN_PASSWORD=... python -m tools.bootstrap --admin-email you@college.edu --admin-name "You" \
      --subject CS501="Operating Systems" \
      --device id=<uuid>,serial=<MAC>,token_hash=<sha256 hex>,name="Desk mic",room="Room 204"

The password is read from the ADMIN_PASSWORD environment variable, never from the command line.
More accounts can be given in BOOTSTRAP_USERS, a JSON list of {"email","name","role","password"}
(role lecturer, student or admin); lecturers and admins among them are also assigned the devices.
A device keeps the id and token it was provisioned with: pass the SHA-256 of its token (only the
hash is stored), so a recorder moved to a new server needs no re-flashing. The admin is assigned
to every device given here. Safe to re-run: existing rows are left as they are.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import re
import sys
import uuid

from sqlalchemy import func, select
from sqlalchemy.ext.asyncio import AsyncSession

from app.api.security import hash_password
from app.db.models import Device, DeviceLecturer, Role, Subject, User
from app.db.session import get_engine, get_sessionmaker


def parse_device(text: str) -> dict[str, str]:
    fields = dict(part.split("=", 1) for part in re.findall(r'(\w+=(?:"[^"]*"|[^,]*))', text))
    fields = {k: v.strip('"') for k, v in fields.items()}
    missing = {"id", "serial", "token_hash"} - fields.keys()
    if missing:
        raise SystemExit(f"--device needs {', '.join(sorted(missing))}")
    if not re.fullmatch(r"[0-9a-f]{64}", fields["token_hash"]):
        raise SystemExit("token_hash must be 64 lowercase hex characters (SHA-256)")
    return fields


async def _user(db: AsyncSession, email: str, name: str, role: Role, password: str) -> User:
    email = email.strip().lower()
    user = (await db.execute(select(User).where(func.lower(User.email) == email))).scalars().first()
    if user is None:
        user = User(email=email, name=name, role=role, password_hash=hash_password(password))
        db.add(user)
        await db.flush()
        print(f"created {role.value} {email}")
    else:
        print(f"exists: {email}")
    return user


def parse_users(raw: str) -> list[dict[str, str]]:
    users = json.loads(raw) if raw.strip() else []
    for u in users:
        if not {"email", "role", "password"} <= u.keys() or u["role"] not in {r.value for r in Role}:
            raise SystemExit("BOOTSTRAP_USERS entries need email, role (lecturer|student|admin), password")
        if len(u["password"]) < 8:
            raise SystemExit(f"password for {u['email']} is shorter than 8 characters")
    return users


async def run(args: argparse.Namespace, password: str, extra: list[dict[str, str]] | None = None) -> None:
    async with get_sessionmaker()() as db:
        admin = await _user(db, args.admin_email, args.admin_name, Role.admin, password)
        staff = [admin]
        for u in extra or []:
            user = await _user(db, u["email"], u.get("name") or u["email"], Role(u["role"]), u["password"])
            if user.role != Role.student:
                staff.append(user)
        for spec in args.subject:
            code, _, name = spec.partition("=")
            if not (await db.execute(select(Subject).where(Subject.code == code))).scalars().first():
                db.add(Subject(code=code, name=name.strip('"') or code, lecturer_id=admin.id))
                print(f"created subject {code}")
        for spec in args.device:
            fields = parse_device(spec)
            device_id = uuid.UUID(fields["id"])
            if await db.get(Device, device_id) is None:
                db.add(
                    Device(
                        id=device_id,
                        name=fields.get("name", "Recorder"),
                        room=fields.get("room", ""),
                        serial=fields["serial"].upper(),
                        token_hash=fields["token_hash"],
                    )
                )
                await db.flush()
                print(f"created device {device_id}")
            else:
                print(f"exists: device {device_id}")
            linked = set(
                (
                    await db.execute(
                        select(DeviceLecturer.user_id).where(DeviceLecturer.device_id == device_id)
                    )
                ).scalars()
            )
            for user in staff:
                if user.id not in linked:
                    db.add(DeviceLecturer(device_id=device_id, user_id=user.id))
        await db.commit()
    await get_engine().dispose()


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--admin-email", required=True)
    parser.add_argument("--admin-name", default="Administrator")
    parser.add_argument("--subject", action="append", default=[], help='CODE="Name" (repeatable)')
    parser.add_argument("--device", action="append", default=[], help="id=,serial=,token_hash=,name=,room=")
    args = parser.parse_args()
    password = os.environ.get("ADMIN_PASSWORD", "")
    if len(password) < 8:
        print("error: set ADMIN_PASSWORD (at least 8 characters)", file=sys.stderr)
        return 1
    asyncio.run(run(args, password, parse_users(os.environ.get("BOOTSTRAP_USERS", ""))))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
