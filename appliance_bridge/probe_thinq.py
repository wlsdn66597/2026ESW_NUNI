"""Explicit read-only real-cloud probe, or credential-free local SDK mock check."""
import argparse
import asyncio
import json
import os
import uuid
from aiohttp import ClientSession
from thinqconnect.thinq_api import ThinQApi
from thinq_mock import ThinQMock
from rule import ROLES


async def main(real):
    if not real:
        async with ThinQMock() as api:
            for role in ROLES:
                for on in (False, True):
                    print(json.dumps({"role": role, "on": on, "result": await api.control(role, on)}))
        return
    token = os.environ.get("THINQ_PAT")
    if not token: raise SystemExit("THINQ_PAT is required for --list; never put the token in source or chat.")
    async with ClientSession() as session:
        api = ThinQApi(session, token, "KR", os.environ.get("THINQ_CLIENT_ID") or str(uuid.uuid4()))
        devices = await api.async_get_device_list()
        if not isinstance(devices, list): raise RuntimeError("unexpected device-list response")
        print(json.dumps({"cloud_read_success": True, "device_count": len(devices), "control_sent": False}))


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__); p.add_argument("--list", action="store_true")
    asyncio.run(main(p.parse_args().list))
