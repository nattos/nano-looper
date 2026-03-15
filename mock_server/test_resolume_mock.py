"""Tests for the mock Resolume WebSocket server."""

import json

import pytest
import pytest_asyncio
from aiohttp import web

from resolume_mock import ResolumeState, create_app, FIXTURE_PATH


@pytest.fixture
def initial_state():
    with open(FIXTURE_PATH) as f:
        return json.load(f)


@pytest.fixture
def state(initial_state):
    return ResolumeState(initial_state)


@pytest.fixture
def app(state):
    return create_app(state)


@pytest_asyncio.fixture
async def client(aiohttp_client, app):
    return await aiohttp_client(app)


# --- ResolumeState unit tests ---


class TestResolumeState:
    def test_find_by_id(self, state):
        # ID for composition name
        param = state.find_by_id(1763903988784)
        assert param is not None
        assert param["valuetype"] == "ParamString"
        assert param["value"] == "Probe Test"

    def test_find_by_id_not_found(self, state):
        assert state.find_by_id(999999) is None

    def test_path_for_id(self, state):
        path = state.path_for_id(1763903988784)
        assert path == "/composition/name"

    def test_find_layer_opacity_by_id(self, state):
        param = state.find_by_id(1763903991101)
        assert param is not None
        assert param["valuetype"] == "ParamRange"
        path = state.path_for_id(1763903991101)
        assert path == "/composition/layers/1/video/opacity"

    def test_find_by_path(self, state):
        param = state.find_by_path("/composition/layers/1/video/opacity")
        assert param is not None
        assert param["id"] == 1763903991101
        assert param["valuetype"] == "ParamRange"

    def test_find_by_path_connected(self, state):
        param = state.find_by_path("/composition/layers/1/clips/1/connected")
        assert param is not None
        assert param["valuetype"] == "ParamState"
        assert param["id"] == 1763903990340

    def test_find_by_path_not_found(self, state):
        assert state.find_by_path("/composition/nonexistent/path") is None


# --- WebSocket tests ---


class TestWebSocket:
    @pytest.mark.asyncio
    async def test_initial_state_on_connect(self, client):
        ws = await client.ws_connect("/api/v1")
        msg = await ws.receive_json()
        assert "layers" in msg
        assert "decks" in msg
        assert "crossfader" in msg
        await ws.close()

    @pytest.mark.asyncio
    async def test_subscribe(self, client):
        ws = await client.ws_connect("/api/v1")
        await ws.receive_json()  # skip initial state

        await ws.send_json({
            "action": "subscribe",
            "parameter": "/parameter/by-id/1763903991101",
        })
        resp = await ws.receive_json()
        assert resp["type"] == "parameter_subscribed"
        assert resp["id"] == 1763903991101
        assert resp["valuetype"] == "ParamRange"
        assert resp["path"] == "/composition/layers/1/video/opacity"
        assert "value" in resp
        await ws.close()

    @pytest.mark.asyncio
    async def test_subscribe_invalid_id(self, client):
        ws = await client.ws_connect("/api/v1")
        await ws.receive_json()

        await ws.send_json({
            "action": "subscribe",
            "parameter": "/parameter/by-id/999999",
        })
        resp = await ws.receive_json()
        assert "error" in resp
        await ws.close()

    @pytest.mark.asyncio
    async def test_set_param_range(self, client):
        ws = await client.ws_connect("/api/v1")
        await ws.receive_json()

        # Subscribe first
        await ws.send_json({
            "action": "subscribe",
            "parameter": "/parameter/by-id/1763903991101",
        })
        await ws.receive_json()  # parameter_subscribed

        # Set value
        await ws.send_json({
            "action": "set",
            "parameter": "/composition/layers/1/video/opacity",
            "id": 1763903991101,
            "value": 0.25,
        })

        resp = await ws.receive_json()
        assert resp["type"] == "parameter_update"
        assert resp["id"] == 1763903991101
        assert resp["value"] == 0.25
        await ws.close()

    @pytest.mark.asyncio
    async def test_set_read_only_trigger(self, client):
        ws = await client.ws_connect("/api/v1")
        await ws.receive_json()

        # Try to set a ParamTrigger — should fail
        await ws.send_json({
            "action": "set",
            "parameter": "/composition/selected",
            "id": 1763903988785,
            "value": True,
        })
        resp = await ws.receive_json()
        assert "error" in resp
        assert "read-only" in resp["error"]
        await ws.close()

    @pytest.mark.asyncio
    async def test_trigger_by_clip_id(self, client):
        ws = await client.ws_connect("/api/v1")
        await ws.receive_json()

        # Subscribe to the connected param
        await ws.send_json({
            "action": "subscribe",
            "parameter": "/parameter/by-id/1763903990340",
        })
        await ws.receive_json()  # parameter_subscribed

        # Trigger clip connect using /clips/by-id/<clip_id>/connect (true+false pulse)
        await ws.send_json({
            "action": "trigger",
            "parameter": "/composition/clips/by-id/1762589641191/connect",
            "value": True,
        })

        resp = await ws.receive_json()
        assert resp["type"] == "parameter_update"
        assert resp["value"] == "Connected"

        # Send the false half of the pulse (should be silently accepted)
        await ws.send_json({
            "action": "trigger",
            "parameter": "/composition/clips/by-id/1762589641191/connect",
            "value": False,
        })

        await ws.close()

    @pytest.mark.asyncio
    async def test_trigger_by_path(self, client):
        ws = await client.ws_connect("/api/v1")
        await ws.receive_json()

        # Subscribe to the connected param
        await ws.send_json({
            "action": "subscribe",
            "parameter": "/parameter/by-id/1763903990340",
        })
        await ws.receive_json()  # parameter_subscribed

        # Also works via layer/clip path (connect → connected alias)
        await ws.send_json({
            "action": "trigger",
            "parameter": "/composition/layers/1/clips/1/connect",
            "value": True,
        })

        resp = await ws.receive_json()
        assert resp["type"] == "parameter_update"
        assert resp["value"] == "Connected"
        await ws.close()

    @pytest.mark.asyncio
    async def test_missing_action(self, client):
        ws = await client.ws_connect("/api/v1")
        await ws.receive_json()

        await ws.send_json({"parameter": "/some/path"})
        resp = await ws.receive_json()
        assert "error" in resp
        assert "action" in resp["error"]
        await ws.close()

    @pytest.mark.asyncio
    async def test_invalid_action(self, client):
        ws = await client.ws_connect("/api/v1")
        await ws.receive_json()

        await ws.send_json({"action": "invalid_action"})
        resp = await ws.receive_json()
        assert "error" in resp
        assert "Invalid enum" in resp["error"]
        await ws.close()

    @pytest.mark.asyncio
    async def test_broadcast_to_multiple_clients(self, client):
        ws1 = await client.ws_connect("/api/v1")
        await ws1.receive_json()
        ws2 = await client.ws_connect("/api/v1")
        await ws2.receive_json()

        # Both subscribe to same param
        for ws in (ws1, ws2):
            await ws.send_json({
                "action": "subscribe",
                "parameter": "/parameter/by-id/1763903991101",
            })
            await ws.receive_json()  # parameter_subscribed

        # Set from ws1
        await ws1.send_json({
            "action": "set",
            "parameter": "/composition/layers/1/video/opacity",
            "id": 1763903991101,
            "value": 0.42,
        })

        # Both should receive update
        resp1 = await ws1.receive_json()
        resp2 = await ws2.receive_json()
        assert resp1["type"] == "parameter_update"
        assert resp1["value"] == 0.42
        assert resp2["type"] == "parameter_update"
        assert resp2["value"] == 0.42

        await ws1.close()
        await ws2.close()


# --- HTTP endpoint tests ---


class TestHTTPEndpoints:
    @pytest.mark.asyncio
    async def test_product_info(self, client):
        resp = await client.get("/api/v1/product")
        assert resp.status == 200
        data = await resp.json()
        assert data["name"] == "Mock Arena"
        assert data["major"] == 7

    @pytest.mark.asyncio
    async def test_product_info_legacy_path(self, client):
        resp = await client.get("/api/product")
        assert resp.status == 200
        data = await resp.json()
        assert data["name"] == "Mock Arena"
