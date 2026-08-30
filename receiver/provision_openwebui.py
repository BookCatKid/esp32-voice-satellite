"""Provision the least-privilege Open WebUI identity used by the satellite.

Run this inside the Open WebUI container with its normal backend environment,
and redirect stdout directly into a protected temporary file. The only output
is the signed service token.
"""

import asyncio

from open_webui.models.access_grants import AccessGrants
from open_webui.models.models import ModelForm, Models
from open_webui.models.users import Users
from open_webui.utils.auth import create_token


USER_ID = "service-voice-satellite"
WORKSPACE_ID = "voice-assistant-ui"
BASE_MODEL_ID = "voice-assistant"


async def ensure_user():
    user = await Users.get_user_by_id(USER_ID)
    if user is None:
        user = await Users.insert_new_user(
            USER_ID,
            "Voice Satellite",
            "voice-satellite@local.invalid",
            role="user",
            username="voice-satellite",
        )
    if user is None:
        raise RuntimeError("could not create Voice Satellite service user")
    # API-key auth is intentionally not required for this integration.
    await Users.delete_user_api_key_by_id(USER_ID)
    return user


async def ensure_model_access():
    workspace = await Models.get_model_by_id(WORKSPACE_ID)
    if workspace is None:
        raise RuntimeError(f"Open WebUI workspace {WORKSPACE_ID!r} does not exist")

    base = await Models.get_model_by_id(BASE_MODEL_ID)
    if base is None:
        base = await Models.insert_new_model(
            ModelForm(
                id=BASE_MODEL_ID,
                base_model_id=None,
                name="Voice Assistant Base",
                params={},
                meta={},
                access_grants=[],
                is_active=True,
            ),
            workspace.user_id,
        )
        if base is None:
            raise RuntimeError(f"could not register base-model metadata for {BASE_MODEL_ID!r}")

    for model_id in (BASE_MODEL_ID, WORKSPACE_ID):
        await AccessGrants.grant_access(
            resource_type="model",
            resource_id=model_id,
            principal_type="user",
            principal_id=USER_ID,
            permission="read",
        )


async def main():
    await ensure_user()
    await ensure_model_access()
    # Do not add labels or logging to stdout: callers intentionally redirect
    # this one value directly into a protected env file.
    print(create_token({"id": USER_ID}))


asyncio.run(main())
