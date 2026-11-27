"""Runs PostgreSQL (embedded, pgserver) for a laptop demo, and checks that Redis is reachable.

  docker run -d --name lc-redis --restart unless-stopped -p 127.0.0.1:6391:6379 redis:7-alpine
  python -m tools.local_services            # keeps running; Ctrl+C stops PostgreSQL's watcher

Data is kept in backend/data/pg between runs. It writes backend/.env.local-services with the
DATABASE_URL and REDIS_URL to copy into backend/.env.

Redis must be a real Redis 5+ (Docker above, Memurai, or WSL). fakeredis's TCP server does not
deliver pub/sub messages between processes, so the web app's Start/Pause/Stop buttons would
never reach the recorder. Not for production: use PostgreSQL 16 + pgvector and Redis 7 (infra/).
"""

from __future__ import annotations

import argparse
import re
import socket
import time
from pathlib import Path

DATA = Path(__file__).resolve().parent.parent / "data" / "pg"


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--redis-port", type=int, default=6391)
    args = parser.parse_args()

    import pgserver

    DATA.mkdir(parents=True, exist_ok=True)
    server = pgserver.get_server(DATA, cleanup_mode=None)  # type: ignore[attr-defined]
    exists = server.psql("SELECT 1 FROM pg_database WHERE datname = 'smartclass';")
    if "(1 row)" not in exists:
        server.psql("CREATE DATABASE smartclass;")
    uri = server.get_uri("smartclass")
    async_uri = re.sub(r"^postgresql(\+\w+)?://", "postgresql+asyncpg://", uri)
    redis_url = f"redis://127.0.0.1:{args.redis_port}/0"
    try:
        socket.create_connection(("127.0.0.1", args.redis_port), timeout=2).close()
        redis_note = "reachable"
    except OSError:
        redis_note = "NOT reachable: start it with the docker command in this file's docstring"
    env = Path(__file__).resolve().parent.parent / ".env.local-services"
    env.write_text(f"DATABASE_URL={async_uri}\nREDIS_URL={redis_url}\n")
    print(f"PostgreSQL: {async_uri}\nRedis:      {redis_url} ({redis_note})\nWrote {env}", flush=True)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
