import asyncio
import unittest

from audio import SATELLITE_FORMAT
from speaker import SatelliteSpeaker


class FakeSocket:
    def __init__(self):
        self.closed = False
        self.messages = []

    async def send_json(self, payload):
        self.messages.append(("json", payload))

    async def send_bytes(self, payload):
        self.messages.append(("bytes", payload))


class SpeakerTest(unittest.IsolatedAsyncioTestCase):
    async def test_playback_is_framed_and_terminated(self):
        socket = FakeSocket()
        speaker = SatelliteSpeaker(volume=100)
        await speaker.bind(socket, supported=True)
        pcm = bytes(SATELLITE_FORMAT.frame_bytes * 2)

        self.assertTrue(await speaker.play(pcm))
        await asyncio.sleep(0.08)

        kinds = [kind for kind, _ in socket.messages]
        self.assertEqual(kinds, ["json", "bytes", "bytes", "json"])
        self.assertEqual(socket.messages[0][1]["type"], "playback.start")
        self.assertEqual(socket.messages[-1][1]["type"], "playback.end")

    async def test_unbind_cancels_active_playback(self):
        socket = FakeSocket()
        speaker = SatelliteSpeaker()
        await speaker.bind(socket, supported=True)
        await speaker.play(bytes(SATELLITE_FORMAT.frame_bytes * 20))

        await speaker.unbind(socket)

        self.assertFalse(speaker.connected)
        self.assertFalse(speaker.playing)
        self.assertIn("playback.cancel", [
            payload.get("type") for kind, payload in socket.messages if kind == "json"
        ])


if __name__ == "__main__":
    unittest.main()
