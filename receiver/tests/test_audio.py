import unittest

import numpy as np

from audio import (
    PIPELINE_FORMAT,
    SATELLITE_FORMAT,
    apply_volume,
    frame_level,
    resample_frame,
)


class AudioTest(unittest.TestCase):
    def test_wire_frame_resamples_to_one_pipeline_frame(self):
        source = np.arange(SATELLITE_FORMAT.frame_samples, dtype="<i2").tobytes()
        output = resample_frame(source)
        self.assertEqual(len(output), PIPELINE_FORMAT.frame_bytes)
        samples = np.frombuffer(output, dtype="<i2")
        self.assertEqual(samples[0], 0)
        self.assertGreater(samples[-1], 430)

    def test_volume_is_linear_and_clamped(self):
        pcm = np.array([-20000, 10000], dtype="<i2").tobytes()
        self.assertEqual(
            np.frombuffer(apply_volume(pcm, 16), dtype="<i2").tolist(),
            [-3200, 1600],
        )
        self.assertEqual(apply_volume(pcm, 0), bytes(len(pcm)))
        self.assertEqual(apply_volume(pcm, 200), pcm)

    def test_silence_has_stable_floor(self):
        silence = bytes(PIPELINE_FORMAT.frame_bytes)
        dbfs, peak_dbfs = frame_level(silence)
        self.assertAlmostEqual(dbfs, -90.3087, places=3)
        self.assertAlmostEqual(peak_dbfs, -90.3087, places=3)


if __name__ == "__main__":
    unittest.main()
