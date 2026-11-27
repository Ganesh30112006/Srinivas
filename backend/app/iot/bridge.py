"""Device links over AWS IoT Core (MQTT).

Devices speak the same protocol as on the WebSocket gateway (contracts/PROTOCOL.md §1), carried
on two MQTT topics per device:

- `lc/<device_id>/up`: device → server. JSON control messages and binary audio frames (a frame
  starts with the bytes `LC`, JSON with `{`). The IoT policy lets a device publish only to its
  own topic, so the topic names the device; `hello` must agree with it.
- `lc/<device_id>/down`: server → device, JSON only.

Each device gets an `MqttDeviceSocket` that looks like a WebSocket to `DeviceConnection`, so the
storage, ACK, gap and live-event logic is the gateway's, unchanged. MQTT has no close frame: a
WebSocket close code that the device must act on is sent as `{"type":"close","code":N}`.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import time
import uuid
from collections.abc import Awaitable, Callable, Mapping
from typing import Any

from starlette.websockets import WebSocketDisconnect

from app.gateway import protocol
from app.logs import get_logger

log = get_logger("iot")

Publish = Callable[[str, bytes], Awaitable[None]]
StartConnection = Callable[["MqttDeviceSocket"], Awaitable[None]]

IDLE_TIMEOUT_S = 40.0  # devices send `status` every 5 s (1 s while recording)
RECONNECT_HINT_INTERVAL_S = 10.0
# Codes the device must see; 4409 (replaced) is internal: the new link is the same device.
FORWARDED_CLOSE_CODES = {protocol.CLOSE_AUTH, protocol.CLOSE_PROTOCOL}
CLOSE_RECONNECT = 4000  # "say hello again": the server has no link for this device


class MqttDeviceSocket:
    """One device's MQTT conversation, presented with the WebSocket methods DeviceConnection uses."""

    def __init__(self, device_id: uuid.UUID, down_topic: str, publish: Publish) -> None:
        self.device_id = device_id
        self.down_topic = down_topic
        self._publish = publish
        self._inbox: asyncio.Queue[dict[str, Any]] = asyncio.Queue()
        self.closed = False
        self.last_rx = time.monotonic()

    # fed by the bridge
    def feed_text(self, text: str) -> None:
        self.last_rx = time.monotonic()
        self._inbox.put_nowait({"type": "websocket.receive", "text": text})

    def feed_bytes(self, data: bytes) -> None:
        self.last_rx = time.monotonic()
        self._inbox.put_nowait({"type": "websocket.receive", "bytes": data})

    def disconnect(self) -> None:
        if not self.closed:
            self.closed = True
            self._inbox.put_nowait({"type": "websocket.disconnect", "code": 1006})

    # used by DeviceConnection
    async def accept(self) -> None:
        return None

    async def receive(self) -> Mapping[str, Any]:
        return await self._inbox.get()

    async def receive_text(self) -> str:
        message = await self._inbox.get()
        if message["type"] == "websocket.disconnect":
            raise WebSocketDisconnect(1006)
        text = message.get("text")
        if text is None:
            raise RuntimeError("expected a text message")
        return str(text)

    async def send_text(self, data: str) -> None:
        if not self.closed:
            await self._publish(self.down_topic, data.encode())

    async def close(self, code: int = 1000, reason: str | None = None) -> None:
        if code in FORWARDED_CLOSE_CODES:
            with contextlib.suppress(Exception):
                await self._publish(self.down_topic, json.dumps({"type": "close", "code": code}).encode())
        self.disconnect()


class Bridge:
    """Routes MQTT messages to per-device links and runs a DeviceConnection for each."""

    def __init__(self, prefix: str, publish: Publish, start_connection: StartConnection) -> None:
        self.prefix = prefix
        self._publish = publish
        self._start = start_connection
        self.links: dict[uuid.UUID, MqttDeviceSocket] = {}
        self._tasks: set[asyncio.Task[None]] = set()
        self._last_hint: dict[uuid.UUID, float] = {}

    @property
    def subscription(self) -> str:
        return f"{self.prefix}/+/up"

    def _device_from_topic(self, topic: str) -> uuid.UUID | None:
        parts = topic.split("/")
        if len(parts) != 3 or parts[0] != self.prefix or parts[2] != "up":
            return None
        try:
            return uuid.UUID(parts[1])
        except ValueError:
            return None

    def _down(self, device_id: uuid.UUID) -> str:
        return f"{self.prefix}/{device_id}/down"

    async def on_message(self, topic: str, payload: bytes) -> None:
        device_id = self._device_from_topic(topic)
        if device_id is None or not payload:
            return
        if payload[:1] == b"{":
            await self._on_json(device_id, payload)
            return
        link = self.links.get(device_id)
        if link is None or link.closed:
            await self._hint_reconnect(device_id)
            return
        link.feed_bytes(payload)

    async def _on_json(self, device_id: uuid.UUID, payload: bytes) -> None:
        try:
            text = payload.decode()
            kind = json.loads(text).get("type")
        except (UnicodeDecodeError, ValueError, AttributeError):
            return
        link = self.links.get(device_id)
        if kind == "bye":  # the device's MQTT last will: it dropped off
            if link is not None:
                log.info("iot_device_gone", device_id=str(device_id))
                link.disconnect()
            return
        if kind == "hello":
            await self._on_hello(device_id, text)
            return
        if link is None or link.closed:
            await self._hint_reconnect(device_id)
            return
        link.feed_text(text)

    async def _on_hello(self, device_id: uuid.UUID, text: str) -> None:
        try:
            claimed = uuid.UUID(str(json.loads(text).get("device_id")))
        except (ValueError, TypeError):
            claimed = None
        if claimed != device_id:  # the topic is what IoT Core authenticated
            log.warning("iot_hello_mismatch", topic_device=str(device_id), hello_device=str(claimed))
            await self._send(
                device_id, protocol.error_msg("auth_failed", "device id does not match certificate")
            )
            await self._send(device_id, {"type": "close", "code": protocol.CLOSE_AUTH})
            return
        old = self.links.get(device_id)
        if old is not None:
            old.disconnect()
        link = MqttDeviceSocket(device_id, self._down(device_id), self._publish)
        self.links[device_id] = link
        link.feed_text(text)
        task = asyncio.create_task(self._run(link))
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)

    async def _run(self, link: MqttDeviceSocket) -> None:
        try:
            await self._start(link)
        except Exception:
            log.exception("iot_connection_failed", device_id=str(link.device_id))
        finally:
            link.disconnect()
            if self.links.get(link.device_id) is link:
                del self.links[link.device_id]

    async def _hint_reconnect(self, device_id: uuid.UUID) -> None:
        """Traffic from a device we hold no link for (e.g. after a bridge restart): ask it to
        reconnect and say hello, at most every few seconds."""
        now = time.monotonic()
        if now - self._last_hint.get(device_id, 0.0) < RECONNECT_HINT_INTERVAL_S:
            return
        self._last_hint[device_id] = now
        await self._send(device_id, {"type": "close", "code": CLOSE_RECONNECT})

    async def _send(self, device_id: uuid.UUID, message: dict[str, Any]) -> None:
        with contextlib.suppress(Exception):
            await self._publish(self._down(device_id), json.dumps(message).encode())

    def expire_idle(self) -> None:
        """A device that stopped talking without a last will (rare) is treated as disconnected."""
        now = time.monotonic()
        for link in list(self.links.values()):
            if now - link.last_rx > IDLE_TIMEOUT_S:
                log.info("iot_device_idle", device_id=str(link.device_id))
                link.disconnect()

    def disconnect_all(self) -> None:
        for link in list(self.links.values()):
            link.disconnect()

    async def drain(self, timeout_s: float = 10.0) -> None:
        if self._tasks:
            await asyncio.wait(set(self._tasks), timeout=timeout_s)
