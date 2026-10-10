import unittest

from host.imu_lab.protocol import IMUSample, format_sample, parse_line


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.sample = IMUSample(
            seq=12, t_ms=600, ax_g=0.1, ay_g=-0.2, az_g=0.97,
            gx_dps=1.0, gy_dps=2.0, gz_dps=3.0,
            mx_uT=30.0, my_uT=-4.0, mz_uT=41.0,
            roll_deg=2.5, pitch_deg=-1.0, yaw_deg=45.0,
        )

    def test_round_trip(self):
        decoded = parse_line(format_sample(self.sample))
        self.assertEqual(decoded.seq, self.sample.seq)
        self.assertEqual(decoded.t_ms, self.sample.t_ms)
        self.assertAlmostEqual(decoded.roll_deg, self.sample.roll_deg)
        self.assertAlmostEqual(decoded.yaw_deg, self.sample.yaw_deg)

    def test_strips_newline(self):
        decoded = parse_line(format_sample(self.sample) + "\r\n")
        self.assertEqual(decoded.seq, 12)

    def test_rejects_wrong_prefix(self):
        with self.assertRaisesRegex(ValueError, "prefix"):
            parse_line("DATA,1,2")

    def test_rejects_wrong_field_count(self):
        with self.assertRaisesRegex(ValueError, "field count"):
            parse_line("IMU,1,2,3")

    def test_rejects_non_numeric_value(self):
        line = format_sample(self.sample).replace("0.100000", "oops", 1)
        with self.assertRaisesRegex(ValueError, "non-numeric"):
            parse_line(line)

    def test_rejects_nan(self):
        line = format_sample(self.sample).replace("0.100000", "nan", 1)
        with self.assertRaisesRegex(ValueError, "finite"):
            parse_line(line)

    def test_rejects_negative_sequence(self):
        line = format_sample(self.sample).replace("IMU,12,600", "IMU,-1,600")
        with self.assertRaisesRegex(ValueError, "non-negative"):
            parse_line(line)


if __name__ == "__main__":
    unittest.main()
