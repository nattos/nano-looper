"""Mock Resolume WebSocket server for development and testing."""

import argparse
import asyncio
import json
import logging
from pathlib import Path
from typing import Any

import aiohttp
from aiohttp import web

logger = logging.getLogger(__name__)

FIXTURE_PATH = Path(__file__).parent / "fixtures" / "resolume-ws-initial-state.json"


class ResolumeState:
    """Manages mutable composition state and parameter lookups."""

    def __init__(self, initial_state: dict[str, Any]):
        self.state = initial_state
        self._id_cache: dict[int, dict[str, Any]] = {}
        self._path_cache: dict[int, str] = {}
        self._clip_cache: dict[int, dict[str, Any]] = {}
        self._build_id_cache(self.state, "/composition")

    def _build_id_cache(self, obj: Any, path: str) -> None:
        if not isinstance(obj, dict):
            return
        if "id" in obj and "valuetype" in obj:
            param_id = obj["id"]
            self._id_cache[param_id] = obj
            self._path_cache[param_id] = path
        # Also index clips by their object ID (for /clips/by-id/<id>/connect)
        if "id" in obj and "connected" in obj and "clips" not in path.split("/")[-1:]:
            self._clip_cache[obj["id"]] = obj
        for key, value in obj.items():
            if key in ("id", "valuetype", "value", "min", "max", "options", "index",
                       "view", "suffix", "control_type"):
                continue
            if isinstance(value, dict):
                self._build_id_cache(value, f"{path}/{key}")
            elif isinstance(value, list):
                for i, item in enumerate(value):
                    self._build_id_cache(item, f"{path}/{key}/{i + 1}")

    def find_by_id(self, param_id: int) -> dict[str, Any] | None:
        return self._id_cache.get(param_id)

    def path_for_id(self, param_id: int) -> str | None:
        return self._path_cache.get(param_id)

    def find_clip_by_id(self, clip_id: int) -> dict[str, Any] | None:
        return self._clip_cache.get(clip_id)

    def find_by_path(self, path: str) -> dict[str, Any] | None:
        """Resolve a parameter by its canonical path like /composition/layers/1/video/opacity."""
        parts = [p for p in path.split("/") if p]
        if parts and parts[0] == "composition":
            parts = parts[1:]

        current: Any = self.state
        for part in parts:
            if current is None:
                return None
            if isinstance(current, list):
                try:
                    idx = int(part) - 1  # 1-indexed
                    current = current[idx] if 0 <= idx < len(current) else None
                except ValueError:
                    return None
            elif isinstance(current, dict):
                current = current.get(part)
            else:
                return None
        return current if isinstance(current, dict) and "id" in current else None


class ClientSession:
    """Tracks one WebSocket client's subscriptions."""

    def __init__(self, ws: web.WebSocketResponse):
        self.ws = ws
        self.subscribed_ids: set[int] = set()


class ResolumeServer:
    """Mock Resolume WebSocket + HTTP server."""

    def __init__(self, state: ResolumeState):
        self.state = state
        self.clients: list[ClientSession] = []

    async def handle_ws(self, request: web.Request) -> web.WebSocketResponse:
        ws = web.WebSocketResponse()
        await ws.prepare(request)
        client = ClientSession(ws)
        self.clients.append(client)
        logger.info("Client connected")

        # Send initial composition state
        await ws.send_json(self.state.state)

        try:
            async for msg in ws:
                if msg.type == aiohttp.WSMsgType.TEXT:
                    try:
                        data = json.loads(msg.data)
                    except json.JSONDecodeError:
                        await ws.send_json({"error": "Invalid JSON"})
                        continue
                    await self._handle_message(client, data)
                elif msg.type in (aiohttp.WSMsgType.ERROR, aiohttp.WSMsgType.CLOSE):
                    break
        finally:
            self.clients.remove(client)
            logger.info("Client disconnected")
        return ws

    async def _handle_message(self, client: ClientSession, data: dict[str, Any]) -> None:
        action = data.get("action")
        if action is None:
            await client.ws.send_json({
                "id": None,
                "error": 'error reading field "action": mandatory field doesn\'t exist',
            })
            return

        if action == "subscribe":
            await self._handle_subscribe(client, data)
        elif action == "set":
            await self._handle_set(client, data)
        elif action == "trigger":
            await self._handle_trigger(client, data)
        else:
            await client.ws.send_json({
                "id": None,
                "error": 'error reading field "action": Invalid enum value',
            })

    async def _handle_subscribe(self, client: ClientSession, data: dict[str, Any]) -> None:
        parameter = data.get("parameter", "")
        # Expected format: /parameter/by-id/<id>
        prefix = "/parameter/by-id/"
        if not parameter.startswith(prefix):
            await client.ws.send_json({
                "path": parameter,
                "error": "Invalid parameter path",
            })
            return

        try:
            param_id = int(parameter[len(prefix):])
        except ValueError:
            await client.ws.send_json({
                "path": parameter,
                "error": "Invalid parameter ID",
            })
            return

        param = self.state.find_by_id(param_id)
        if param is None:
            await client.ws.send_json({
                "path": parameter,
                "error": "Parameter not found",
            })
            return

        client.subscribed_ids.add(param_id)
        path = self.state.path_for_id(param_id) or parameter

        response: dict[str, Any] = {
            "id": param["id"],
            "valuetype": param["valuetype"],
            "value": param["value"],
            "path": path,
            "type": "parameter_subscribed",
        }
        # Include min/max for ParamRange
        if "min" in param:
            response["min"] = param["min"]
        if "max" in param:
            response["max"] = param["max"]
        await client.ws.send_json(response)

    async def _handle_set(self, client: ClientSession, data: dict[str, Any]) -> None:
        parameter = data.get("parameter", "")
        param_id = data.get("id")
        value = data.get("value")

        if not parameter or param_id is None or value is None:
            await client.ws.send_json({
                "path": parameter or "<unknown>",
                "error": "Missing required fields (parameter, id, value)",
            })
            return

        param = self.state.find_by_id(param_id)
        if param is None:
            await client.ws.send_json({
                "path": parameter,
                "error": "Invalid parameter path",
            })
            return

        # Check if parameter is settable
        if param.get("valuetype") in ("ParamTrigger", "ParamEvent"):
            await client.ws.send_json({
                "path": parameter,
                "error": "This field is read-only",
            })
            return

        param["value"] = value
        # Broadcast update to all subscribers
        await self._broadcast_update(param_id, param, parameter)

    async def _handle_trigger(self, client: ClientSession, data: dict[str, Any]) -> None:
        parameter = data.get("parameter", "")
        value = data.get("value")

        if not parameter or value not in (True, False):
            await client.ws.send_json({
                "path": parameter or "<unknown>",
                "error": "Missing parameter or invalid value for trigger",
            })
            return

        # value=false is the second half of a trigger pulse — acknowledge silently
        if value is False:
            return

        # Handle /composition/clips/by-id/<clip_id>/connect
        import re
        by_id_match = re.match(r"^/composition/clips/by-id/(\d+)/connect$", parameter)
        if by_id_match:
            clip_id = int(by_id_match.group(1))
            clip = self.state.find_clip_by_id(clip_id)
            if clip is None:
                await client.ws.send_json({
                    "path": parameter,
                    "error": "Clip not found",
                })
                return
            param = clip.get("connected")
            if param and param.get("valuetype") == "ParamState":
                param["value"] = "Connected"
                if "index" in param:
                    options = param.get("options", [])
                    param["index"] = options.index("Connected") if "Connected" in options else 3
                # Broadcast using the canonical path from the param's path cache
                broadcast_path = self.state.path_for_id(param["id"]) or parameter
                await self._broadcast_update(param["id"], param, broadcast_path)
            return

        # Fallback: resolve by path (with connect → connected alias)
        param = self.state.find_by_path(parameter)
        if param is None and parameter.endswith("/connect"):
            param = self.state.find_by_path(
                parameter[:-len("/connect")] + "/connected")

        if param is None:
            await client.ws.send_json({
                "path": parameter,
                "error": "Invalid parameter path",
            })
            return

        if param.get("valuetype") == "ParamState":
            param["value"] = "Connected"
            if "index" in param:
                options = param.get("options", [])
                param["index"] = options.index("Connected") if "Connected" in options else 3

        await self._broadcast_update(param["id"], param, parameter)

    async def _broadcast_update(
        self, param_id: int, param: dict[str, Any], path: str
    ) -> None:
        update = {
            "id": param_id,
            "valuetype": param["valuetype"],
            "value": param["value"],
            "path": path,
            "type": "parameter_update",
        }
        for c in self.clients:
            if param_id in c.subscribed_ids:
                try:
                    await c.ws.send_json(update)
                except ConnectionResetError:
                    pass

    async def handle_product(self, request: web.Request) -> web.Response:
        return web.json_response({
            "name": "Mock Arena",
            "major": 7,
            "minor": 18,
            "micro": 2,
            "revision": 29742,
        })


def create_app(state: ResolumeState | None = None) -> web.Application:
    if state is None:
        with open(FIXTURE_PATH) as f:
            initial = json.load(f)
        state = ResolumeState(initial)

    server = ResolumeServer(state)
    app = web.Application()
    app["server"] = server
    app.router.add_get("/api/v1", server.handle_ws)
    app.router.add_get("/api/v1/product", server.handle_product)
    app.router.add_get("/api/product", server.handle_product)
    return app


def main() -> None:
    parser = argparse.ArgumentParser(description="Mock Resolume WebSocket server")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--fixture", type=str, default=None,
                        help="Path to initial state JSON fixture")
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO)

    if args.fixture:
        with open(args.fixture) as f:
            initial = json.load(f)
        state = ResolumeState(initial)
    else:
        state = None

    app = create_app(state)
    web.run_app(app, host=args.host, port=args.port)


if __name__ == "__main__":
    main()
