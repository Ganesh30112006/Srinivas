"""AWS IoT Core bridge: `python -m app.iot.main`.

Connects to AWS IoT Core with the backend's own certificate, subscribes to every device's `up`
topic and runs the gateway's DeviceConnection for each device (see app/iot/bridge.py). Runs next
to (or instead of) the WebSocket gateway; it also runs the stale-session finaliser.
"""

from __future__ import annotations

import asyncio
import contextlib
import signal
import ssl
import sys

import aiomqtt
import certifi

from app.audio.storage import get_audio_store
from app.config import Settings, get_settings
from app.db.session import get_sessionmaker
from app.events import create_redis
from app.gateway.connection import DeviceConnection, Registry
from app.gateway.main import finalize_stale_sessions
from app.iot.bridge import Bridge, MqttDeviceSocket
from app.logs import configure_logging, get_logger

log = get_logger("iot")


def tls_context(settings: Settings) -> ssl.SSLContext:
    for path in (settings.iot_cert_file, settings.iot_key_file):
        if not path.is_file():
            raise SystemExit(f"missing {path}: run `python -m tools.aws_iot backend` first")
    ctx = ssl.create_default_context(cafile=certifi.where())  # includes Amazon Root CA 1
    ctx.load_cert_chain(str(settings.iot_cert_file), str(settings.iot_key_file))
    return ctx


async def run(settings: Settings) -> None:
    if not settings.iot_endpoint:
        raise SystemExit("IOT_ENDPOINT is not set (see `python -m tools.aws_iot backend`)")
    redis = create_redis()
    store = get_audio_store()
    sessions = get_sessionmaker()
    registry = Registry()
    client: aiomqtt.Client | None = None

    async def publish(topic: str, payload: bytes) -> None:
        if client is None:
            raise ConnectionError("not connected to AWS IoT Core")
        await client.publish(topic, payload, qos=1)

    async def start_connection(link: MqttDeviceSocket) -> None:
        await DeviceConnection(link, redis, store, sessions, settings, registry).run()

    bridge = Bridge(settings.iot_topic_prefix, publish, start_connection)
    stale = asyncio.create_task(finalize_stale_sessions(sessions, redis, settings))

    async def idle_check() -> None:
        while True:
            await asyncio.sleep(5)
            bridge.expire_idle()

    idle = asyncio.create_task(idle_check())
    ctx = tls_context(settings)
    delay = 1.0
    try:
        while True:
            try:
                async with aiomqtt.Client(
                    hostname=settings.iot_endpoint,
                    port=8883,
                    identifier=settings.iot_client_id,
                    tls_context=ctx,
                    keepalive=30,
                ) as connected:
                    client = connected
                    await connected.subscribe(bridge.subscription, qos=1)
                    log.info("iot_connected", endpoint=settings.iot_endpoint, topic=bridge.subscription)
                    delay = 1.0
                    async for message in connected.messages:
                        payload = message.payload
                        data = payload if isinstance(payload, bytes | bytearray) else str(payload).encode()
                        await bridge.on_message(message.topic.value, bytes(data))
            except aiomqtt.MqttError as exc:
                log.warning("iot_disconnected", error=str(exc), retry_s=delay)
            client = None
            bridge.disconnect_all()  # devices are told to say hello again once we're back
            await asyncio.sleep(delay)
            delay = min(delay * 2, 30.0)
    finally:
        client = None
        bridge.disconnect_all()
        await bridge.drain()
        await registry.drain()
        for task in (stale, idle):
            task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await task
        await redis.aclose()


def main() -> None:
    configure_logging("iot")
    settings = get_settings()
    if sys.platform == "win32":
        # paho-mqtt needs add_reader(), which the Windows default (Proactor) loop lacks.
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())  # type: ignore[attr-defined,unused-ignore]
    loop = asyncio.new_event_loop()
    task = loop.create_task(run(settings))
    if sys.platform != "win32":
        for sig in (signal.SIGTERM, signal.SIGINT):
            loop.add_signal_handler(sig, task.cancel)
    try:
        loop.run_until_complete(task)
    except (KeyboardInterrupt, asyncio.CancelledError):
        task.cancel()
        with contextlib.suppress(BaseException):
            loop.run_until_complete(task)
    finally:
        loop.close()


if __name__ == "__main__":
    main()
