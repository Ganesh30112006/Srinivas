"""Every failure answers {code, message} with a message for the user, never a bare 500 page."""

from __future__ import annotations

import pytest
from fastapi import FastAPI, WebSocket
from fastapi.testclient import TestClient
from pydantic import BaseModel, Field
from redis import exceptions as redis_errors
from sqlalchemy import exc as db_errors
from starlette.websockets import WebSocketDisconnect

from app.api.errors import INTERNAL_MESSAGE, UNAVAILABLE_MESSAGE, install_error_handlers


class Body(BaseModel):
    lecture_title: str = Field(min_length=3)
    language: str


def make_app() -> FastAPI:
    app = FastAPI()
    install_error_handlers(app)

    @app.get("/crash")
    async def crash() -> None:
        raise RuntimeError("bug")

    @app.get("/database-down")
    async def database_down() -> None:
        raise db_errors.OperationalError("SELECT 1", {}, ConnectionRefusedError("refused"))

    @app.get("/redis-down")
    async def redis_down() -> None:
        raise redis_errors.ConnectionError("Error 10061 connecting to localhost:6379")

    @app.post("/validate")
    async def validate(body: Body) -> None:
        return None

    @app.websocket("/ws")
    async def socket(ws: WebSocket) -> None:
        await ws.accept()
        raise redis_errors.ConnectionError("lost")

    return app


@pytest.fixture
def client() -> TestClient:
    return TestClient(make_app(), raise_server_exceptions=False)


def test_an_unexpected_crash_is_a_json_error_with_a_plain_message(client: TestClient) -> None:
    response = client.get("/crash")
    assert response.status_code == 500
    assert response.json() == {"code": "internal_error", "message": INTERNAL_MESSAGE}


@pytest.mark.parametrize("path", ["/database-down", "/redis-down"])
def test_an_unreachable_database_or_redis_is_a_503(client: TestClient, path: str) -> None:
    response = client.get(path)
    assert response.status_code == 503
    assert response.json() == {"code": "service_unavailable", "message": UNAVAILABLE_MESSAGE}


def test_validation_errors_name_the_field_in_plain_words(client: TestClient) -> None:
    response = client.post("/validate", json={"lecture_title": "ab"})
    assert response.status_code == 422
    body = response.json()
    assert body["code"] == "validation_error"
    assert body["message"] == "Lecture title: should have at least 3 characters; Language: is required"


def test_an_unknown_address_is_a_json_404(client: TestClient) -> None:
    response = client.get("/no-such-page")
    assert response.status_code == 404
    assert response.json()["code"] == "http_404"


def test_a_socket_is_closed_as_a_server_error_so_the_browser_reconnects(client: TestClient) -> None:
    with client.websocket_connect("/ws") as ws, pytest.raises(WebSocketDisconnect) as closed:
        ws.receive_text()
    assert closed.value.code == 1011
