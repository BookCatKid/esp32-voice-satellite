import asyncio

from open_webui.models.users import Users
from open_webui.utils.auth import create_token


USER_ID = "service-voice-satellite"


async def main():
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

    # The Open WebUI instance has API-key auth disabled globally, so use the
    # normal signed session-token path instead. This avoids changing global
    # authentication settings just for the satellite service.
    await Users.delete_user_api_key_by_id(USER_ID)
    token = create_token({"id": USER_ID})
    print(token)


asyncio.run(main())
