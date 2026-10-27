"""Calls the official SDK's local mock branch. It never issues cloud control."""
import uuid
from aiohttp import ClientSession
from thinqconnect.thinq_api import ThinQApi
from rule import ROLES


def payload_for(role, on):
    if role not in ROLES or type(on) is not bool:
        raise ValueError("unknown role or non-boolean operation")
    key = "airConOperationMode" if role == "air_conditioner" else "airPurifierOperationMode"
    return {"operation": {key: "POWER_ON" if on else "POWER_OFF"}}


class ThinQMock:
    async def __aenter__(self):
        self.session = ClientSession()
        # Deliberately ignore THINQ_PAT: this service can only use local mocks.
        self.api = ThinQApi(self.session, "MOCK_NO_CREDENTIALS", "KR", str(uuid.uuid4()), mock_response=True)
        return self

    async def control(self, role, on):
        payload = payload_for(role, on)
        result = await self.api.async_post_device_control("demo-esp-" + role, payload)
        if result != {"message": "Mock Response", "body": payload}:
            raise RuntimeError("unexpected ThinQ mock response")
        return {"mode": "sdk_mock", "cloud_confirmed": False, "payload": payload, "response": result}

    async def __aexit__(self, *args):
        await self.session.close()
