import tempfile
from pathlib import Path
import unittest

from host.imu_lab.attitude_protocol import AttitudeSample
from host.imu_lab.csvlog import CsvRecord
from host.imu_lab.reporting import analyze_records
from host.report import render_markdown


def record(t: float, pitch: float, roll: float) -> CsvRecord:
    return CsvRecord(
        timestamp_utc=f"t{t}",
        elapsed_s=t,
        sample=AttitudeSample(pitch_deg=pitch, roll_deg=roll),
        raw_line=f"P:{pitch},R:{roll}",
    )


class ReportingTests(unittest.TestCase):
    def test_summary_stats_and_sampling_rate(self):
        summary = analyze_records([record(0.0, 0.0, 1.0), record(0.5, 2.0, 3.0), record(1.0, 4.0, 5.0)])
        self.assertEqual(summary.sample_count, 3)
        self.assertAlmostEqual(summary.duration_s, 1.0)
        self.assertAlmostEqual(summary.estimated_hz, 2.0)
        self.assertAlmostEqual(summary.pitch.minimum, 0.0)
        self.assertAlmostEqual(summary.pitch.maximum, 4.0)
        self.assertAlmostEqual(summary.pitch.mean, 2.0)

    def test_large_step_is_flagged_on_correct_channel(self):
        summary = analyze_records([record(0.0, 0, 0), record(0.1, 40, 1), record(0.2, 41, 50)], jump_threshold_deg=30)
        self.assertEqual(len(summary.jumps), 2)
        self.assertTrue(summary.jumps[0].pitch_jump)
        self.assertFalse(summary.jumps[0].roll_jump)
        self.assertFalse(summary.jumps[1].pitch_jump)
        self.assertTrue(summary.jumps[1].roll_jump)

    def test_threshold_must_be_positive(self):
        with self.assertRaisesRegex(ValueError, "greater than zero"):
            analyze_records([record(0, 0, 0)], jump_threshold_deg=0)

    def test_empty_records_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "at least one"):
            analyze_records([])

    def test_markdown_identifies_simulated_data(self):
        summary = analyze_records([record(0.0, 0, 0), record(1.0, 1, 1)])
        text = render_markdown(summary, Path("simulated.csv"), "simulated", "attitude_plot.png")
        self.assertIn("Data provenance:** `simulated`", text)
        self.assertIn("not be presented as hardware measurements", text)
        self.assertIn("attitude_plot.png", text)


if __name__ == "__main__":
    unittest.main()
