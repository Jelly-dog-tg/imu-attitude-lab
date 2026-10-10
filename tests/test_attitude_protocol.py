import unittest

from host.imu_lab.attitude_protocol import parse_attitude_line


class AttitudeProtocolTests(unittest.TestCase):
    def test_parse_current_firmware_format(self):
        sample = parse_attitude_line("P:10.25,R:-5.50\r\n")
        self.assertAlmostEqual(sample.pitch_deg, 10.25)
        self.assertAlmostEqual(sample.roll_deg, -5.50)
        self.assertIsNone(sample.x)
        self.assertIsNone(sample.y)

    def test_parse_optional_experimental_position(self):
        sample = parse_attitude_line("P:1,R:2,X:0.12,Y:-0.34")
        self.assertEqual((sample.pitch_deg, sample.roll_deg, sample.x, sample.y), (1, 2, 0.12, -0.34))

    def test_rejects_missing_pitch_or_roll(self):
        with self.assertRaisesRegex(ValueError, "P and R"):
            parse_attitude_line("P:1,X:2,Y:3")

    def test_rejects_non_numeric(self):
        with self.assertRaisesRegex(ValueError, "not numeric"):
            parse_attitude_line("P:abc,R:2")

    def test_rejects_nan(self):
        with self.assertRaisesRegex(ValueError, "finite"):
            parse_attitude_line("P:nan,R:2")

    def test_rejects_incomplete_position_pair(self):
        with self.assertRaisesRegex(ValueError, "both X and Y"):
            parse_attitude_line("P:1,R:2,X:3")

    def test_rejects_duplicate_field(self):
        with self.assertRaisesRegex(ValueError, "duplicate"):
            parse_attitude_line("P:1,R:2,P:3")


if __name__ == "__main__":
    unittest.main()
