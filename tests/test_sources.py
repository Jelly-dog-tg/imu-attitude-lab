import csv
from pathlib import Path
import tempfile
import unittest

from host.imu_lab.sources import (
    SourceStats,
    iter_csv_samples,
    iter_simulated_samples,
    iter_source_samples,
)


class SourceAdapterTests(unittest.TestCase):
    def test_simulator_yields_expected_count_and_protocol_lines(self):
        samples = list(iter_simulated_samples(duration_s=2, hz=20, seed=42))
        self.assertEqual(len(samples), 40)
        self.assertEqual(samples[0].elapsed_s, 0.0)
        self.assertTrue(samples[0].raw_line.startswith("P:"))
        self.assertIn(",R:", samples[0].raw_line)
        self.assertAlmostEqual(samples[1].elapsed_s, 0.05)

    def test_simulator_is_reproducible_for_same_seed(self):
        first = list(iter_simulated_samples(duration_s=1, hz=10, seed=11))
        second = list(iter_simulated_samples(duration_s=1, hz=10, seed=11))
        self.assertEqual(first, second)

    def test_simulator_rejects_invalid_parameters(self):
        with self.assertRaisesRegex(ValueError, "duration_s"):
            list(iter_simulated_samples(duration_s=0, hz=20))
        with self.assertRaisesRegex(ValueError, "hz"):
            list(iter_simulated_samples(duration_s=1, hz=0))

    def test_csv_adapter_preserves_data_and_counts_bad_rows(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "input.csv"
            with path.open("w", newline="", encoding="utf-8") as file:
                writer = csv.DictWriter(
                    file,
                    fieldnames=["timestamp_utc", "elapsed_s", "pitch_deg", "roll_deg", "x", "y", "raw_line"],
                )
                writer.writeheader()
                writer.writerow({"timestamp_utc": "t0", "elapsed_s": "0", "pitch_deg": "1.5", "roll_deg": "-2.5", "x": "", "y": "", "raw_line": "P:1.5,R:-2.5"})
                writer.writerow({"timestamp_utc": "t1", "elapsed_s": "bad", "pitch_deg": "3", "roll_deg": "4", "x": "", "y": "", "raw_line": "bad"})
            stats = SourceStats()
            samples = list(iter_csv_samples(path, stats=stats))
            self.assertEqual(len(samples), 1)
            self.assertEqual(stats.skipped_csv_rows, 1)
            self.assertEqual(samples[0].sample.pitch_deg, 1.5)
            self.assertEqual(samples[0].sample.roll_deg, -2.5)
            self.assertEqual(samples[0].timestamp_utc, "t0")

    def test_source_factory_rejects_csv_without_input_path(self):
        with self.assertRaisesRegex(ValueError, "input_path"):
            list(iter_source_samples("csv"))


if __name__ == "__main__":
    unittest.main()
