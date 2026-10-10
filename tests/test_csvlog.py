import csv
from pathlib import Path
import tempfile
import unittest

from host.imu_lab.attitude_protocol import AttitudeSample
from host.imu_lab.csvlog import AttitudeCsvWriter, load_csv


class CsvLogTests(unittest.TestCase):
    def test_writer_round_trip(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "nested" / "session.csv"
            sample = AttitudeSample(pitch_deg=10.5, roll_deg=-2.25)
            with AttitudeCsvWriter(path) as writer:
                writer.write(sample, "P:10.5,R:-2.25", elapsed_s=0.5, timestamp_utc="2026-10-10T00:00:00+00:00")
            records, skipped = load_csv(path)
            self.assertEqual(skipped, 0)
            self.assertEqual(len(records), 1)
            self.assertAlmostEqual(records[0].sample.pitch_deg, 10.5)
            self.assertAlmostEqual(records[0].sample.roll_deg, -2.25)
            self.assertEqual(records[0].raw_line, "P:10.5,R:-2.25")

    def test_writer_rejects_negative_elapsed(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "session.csv"
            with AttitudeCsvWriter(path) as writer:
                with self.assertRaisesRegex(ValueError, "elapsed_s"):
                    writer.write(AttitudeSample(0, 0), "P:0,R:0", -1)

    def test_loader_skips_invalid_rows(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "session.csv"
            with path.open("w", newline="", encoding="utf-8") as file:
                writer = csv.DictWriter(file, fieldnames=["timestamp_utc", "elapsed_s", "pitch_deg", "roll_deg", "x", "y", "raw_line"])
                writer.writeheader()
                writer.writerow({"timestamp_utc": "t0", "elapsed_s": "0", "pitch_deg": "1", "roll_deg": "2", "x": "", "y": "", "raw_line": "P:1,R:2"})
                writer.writerow({"timestamp_utc": "t1", "elapsed_s": "oops", "pitch_deg": "1", "roll_deg": "2", "x": "", "y": "", "raw_line": "bad"})
            records, skipped = load_csv(path)
            self.assertEqual(len(records), 1)
            self.assertEqual(skipped, 1)

    def test_loader_rejects_missing_columns(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad.csv"
            path.write_text("pitch_deg,roll_deg\n1,2\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "CSV must contain columns"):
                load_csv(path)


if __name__ == "__main__":
    unittest.main()
