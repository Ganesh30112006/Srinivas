"""The shared Redis client must let blocking reads finish (regression: redis-py 8 socket timeout)."""

from __future__ import annotations

from app.events import MAX_BLOCKING_READ_S, REDIS_SOCKET_TIMEOUT_S, create_redis


def test_socket_timeout_outlasts_the_longest_blocking_read() -> None:
    redis = create_redis()
    timeout = redis.connection_pool.connection_kwargs["socket_timeout"]
    assert timeout == REDIS_SOCKET_TIMEOUT_S
    assert timeout > MAX_BLOCKING_READ_S * 2
