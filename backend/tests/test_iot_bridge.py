"""AWS IoT Core bridge (app/iot/bridge.py): topic routing, hello checks and the WebSocket-like link."""

from __future__ import annotations

import asyncio
import json
import uuid

import pytest
from starlette.websockets import WebSocketDisconnect

from app.iot.bridge import CLOSE_RECONNECT, Bridge, MqttDeviceSocket

DEVICE = uuid.UUID("b2c133a4-9c0b-48a5-8d2d-670930684167")
UP = f"lc/{DEVICE}/up"
DOWN = f"lc/{DEVICE}/down"


class Recorder:
    def __init__(self) -> None:
        self.sent: list[tuple[str, dict[str, object]]] = []
        self.started: list[MqttDeviceSocket] = []
        self.release = asyncio.Event()

    async def publish(self, topic: str, payload: bytes) -> None:
        self.sent.append((topic, json.loads(payload)))

    async def start(self, link: MqttDeviceSocket) -> None:
        self.started.append(link)
        await self.release.wait()


def hello(device: uuid.UUID = DEVICE) -> bytes:
    return json.dumps({"type": "hello", "device_id": str(device), "token": "x" * 32}).encode()


async def test_hello_starts_a_connection_and_routes_later_messages() -> None:
    rec = Recorder()
    bridge = Bridge("lc", rec.publish, rec.start)
    await bridge.on_message(UP, hello())
    await asyncio.sleep(0)
    assert len(rec.started) == 1
    link = rec.started[0]
    assert json.loads(await link.receive_text())["type"] == "hello"
    await bridge.on_message(UP, b'{"type":"status"}')
    await bridge.on_message(UP, b"LC\x01\x01rest-of-frame")
    assert (await link.receive())["text"] == '{"type":"status"}'
    assert (await link.receive())["bytes"] == b"LC\x01\x01rest-of-frame"
    await link.send_text('{"type":"config"}')
    assert rec.sent == [(DOWN, {"type": "config"})]
    rec.release.set()
    await bridge.drain()
    assert DEVICE not in bridge.links


async def test_hello_for_another_device_is_refused() -> None:
    rec = Recorder()
    bridge = Bridge("lc", rec.publish, rec.start)
    await bridge.on_message(UP, hello(uuid.uuid4()))
    assert rec.started == []
    assert [m["type"] for _, m in rec.sent] == ["error", "close"]
    assert rec.sent[1][1]["code"] == 4401


async def test_traffic_without_a_link_asks_the_device_to_reconnect_once() -> None:
    rec = Recorder()
    bridge = Bridge("lc", rec.publish, rec.start)
    await bridge.on_message(UP, b'{"type":"status"}')
    await bridge.on_message(UP, b"LC\x01\x01frame")
    assert rec.sent == [(DOWN, {"type": "close", "code": CLOSE_RECONNECT})]


async def test_last_will_disconnects_the_link() -> None:
    rec = Recorder()
    bridge = Bridge("lc", rec.publish, rec.start)
    await bridge.on_message(UP, hello())
    await asyncio.sleep(0)
    link = rec.started[0]
    await link.receive_text()
    await bridge.on_message(UP, b'{"type":"bye"}')
    assert (await link.receive())["type"] == "websocket.disconnect"
    rec.release.set()
    await bridge.drain()


async def test_a_new_hello_replaces_the_old_link() -> None:
    rec = Recorder()
    bridge = Bridge("lc", rec.publish, rec.start)
    await bridge.on_message(UP, hello())
    await bridge.on_message(UP, hello())
    await asyncio.sleep(0)
    assert len(rec.started) == 2
    assert rec.started[0].closed and not rec.started[1].closed
    assert bridge.links[DEVICE] is rec.started[1]
    rec.release.set()
    await bridge.drain()


@pytest.mark.parametrize("topic", ["lc/not-a-uuid/up", f"other/{DEVICE}/up", f"lc/{DEVICE}/down", "lc/up"])
async def test_foreign_topics_are_ignored(topic: str) -> None:
    rec = Recorder()
    bridge = Bridge("lc", rec.publish, rec.start)
    await bridge.on_message(topic, hello())
    assert rec.started == [] and rec.sent == []


async def test_auth_close_is_forwarded_but_replacement_is_not() -> None:
    rec = Recorder()
    link = MqttDeviceSocket(DEVICE, DOWN, rec.publish)
    await link.close(4409)
    assert rec.sent == []
    link = MqttDeviceSocket(DEVICE, DOWN, rec.publish)
    await link.close(4401)
    assert rec.sent == [(DOWN, {"type": "close", "code": 4401})]
    with pytest.raises(WebSocketDisconnect):
        await link.receive_text()
