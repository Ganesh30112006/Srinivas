"""Uniform error bodies: {code, message} (ErrorResponse). Every failure the API can produce, including
an unexpected crash or a database/Redis outage, answers in this shape with a message written for
the person using the app, so the web client can always say what went wrong."""

from __future__ import annotations

import contextlib
import re
from typing import Any, cast

from fastapi import FastAPI, Request, WebSocket
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from redis import exceptions as redis_errors
from sqlalchemy import exc as db_errors
from starlette.exceptions import HTTPException as StarletteHTTPException
from starlette.requests import HTTPConnection

from app.logs import get_logger

log = get_logger("api")

# A dependency (PostgreSQL, Redis) can't be reached or timed out: temporary, worth retrying.
UNAVAILABLE_ERRORS: tuple[type[Exception], ...] = (
    db_errors.OperationalError,
    db_errors.InterfaceError,
    redis_errors.ConnectionError,
    redis_errors.TimeoutError,
    ConnectionError,
    TimeoutError,
)
UNAVAILABLE_MESSAGE = (
    "The server can't reach its database right now. Try again in a minute; if it keeps happening, "
    "tell your administrator."
)
INTERNAL_MESSAGE = (
    "Something went wrong on the server. Try again; if it keeps happening, tell your administrator."
)


class ApiError(Exception):
    def __init__(self, status: int, code: str, message: str) -> None:
        super().__init__(message)
        self.status = status
        self.code = code
        self.message = message


def not_found(what: str = "lecture") -> ApiError:
    return ApiError(404, f"{what}_not_found", f"This {what} does not exist.")


def forbidden(message: str = "You do not have permission to do this.") -> ApiError:
    return ApiError(403, "forbidden", message)


def _error(status: int, code: str, message: str) -> JSONResponse:
    return JSONResponse({"code": code, "message": message}, status_code=status)


def validation_message(error: dict[str, Any]) -> str:
    """One readable line per invalid field: "Title: should have at least 3 characters"."""
    field = " ".join(str(part) for part in error["loc"][1:]).replace("_", " ").strip()
    message = str(error["msg"])
    message = re.sub(r"^Value error, ", "", message)
    message = re.sub(r"^(String|Input|Value|List|Dictionary) should", "should", message)
    message = "is required" if message == "Field required" else message
    return f"{field[:1].upper()}{field[1:]}: {message}" if field else message


def install_error_handlers(app: FastAPI) -> None:
    @app.exception_handler(ApiError)
    async def api_error(_: Request, exc: ApiError) -> JSONResponse:
        return _error(exc.status, exc.code, exc.message)

    @app.exception_handler(RequestValidationError)
    async def validation_error(_: Request, exc: RequestValidationError) -> JSONResponse:
        return _error(422, "validation_error", "; ".join(validation_message(e) for e in exc.errors()))

    @app.exception_handler(StarletteHTTPException)
    async def http_error(_: Request, exc: StarletteHTTPException) -> JSONResponse:
        return _error(exc.status_code, f"http_{exc.status_code}", str(exc.detail))

    async def unavailable(conn: HTTPConnection, exc: Exception) -> JSONResponse | None:
        log.error("dependency_unavailable", path=conn.url.path, error=repr(exc))
        if isinstance(conn, WebSocket):
            # A socket can't carry a JSON body: close it as a server error (1011), which the
            # browser treats as temporary and reconnects. (A normal close would mean "ended".)
            with contextlib.suppress(Exception):
                await conn.close(1011)
            return None
        return _error(503, "service_unavailable", UNAVAILABLE_MESSAGE)

    for error_type in UNAVAILABLE_ERRORS:
        # One handler for both scopes (Starlette's type expects separate HTTP and socket handlers).
        app.add_exception_handler(error_type, cast(Any, unavailable))

    @app.exception_handler(Exception)
    async def internal_error(request: Request, exc: Exception) -> JSONResponse:
        # Starlette logs the traceback as well; this line ties it to the request.
        log.error("unhandled_error", path=request.url.path, error=repr(exc))
        return _error(500, "internal_error", INTERNAL_MESSAGE)
