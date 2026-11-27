#!/usr/bin/env python3
"""Tests for tools/bench_compare.py, the part of it that needs neither a
recorded session nor a GPU: what counts as a regression, what does not, what
is a usage error and what is only worth a warning.

    python3 -m unittest discover -s tests -p 'bench_compare_tests.py'

The results it reads are built here, small ones holding the phases, the
renderer's passes and the three extras a comparison looks at.
"""

import contextlib
import io
import json
import pathlib
import sys
import tempfile
import unittest

# tools/bench_compare.py sits next to this file's directory, not on the path.
root = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(root / "tools"))

import bench_compare


# A result as the binary writes it: the version of the format, what the run
# was, the phases with their percentiles, the renderer's passes, and the three
# extras.
def result(**overrides):
    """A benchmark result; every field can be replaced by keyword, and
    `phases` takes phase name -> {percentile: milliseconds}."""
    data = {
        "version": 1,
        "recording": "session.hop1",
        "commit": "a1b2c3d",
        "build": "release",
        "gpu": "NVIDIA GeForce RTX 4070",
        "resolution": "1920x1080",
        "preset": "max",
        "rayTracing": False,
        "fpsEquivalent": 120.0,
        "peakRssMb": 800.0,
        "loadIn": {"totalMs": 4000.0, "captureMs": 1000.0, "sceneMs": 2000.0},
        "phases": {
            "frame": {"frames": 600, "mean": 8.0, "p50": 7.5, "p95": 11.0, "p99": 13.0, "max": 28.0},
            "cpu": {"frames": 600, "mean": 4.2, "p50": 4.0, "p95": 5.5, "p99": 6.5, "max": 14.0},
            "pipelined": {"frames": 600, "mean": 6.0, "p50": 5.8, "p95": 8.0, "p99": 10.0, "max": 20.0},
            "scripts": {"frames": 600, "mean": 3.0, "p50": 2.5, "p95": 4.0, "p99": 5.0, "max": 9.0},
            "render": {"frames": 600, "mean": 4.0, "p50": 3.8, "p95": 5.0, "p99": 6.0, "max": 12.0},
            "camera": {"frames": 600, "mean": 0.0, "p50": 0.0, "p95": 0.0, "p99": 0.0, "max": 0.0},
        },
        "passNames": ["depth prepass", "opaque", "sky", "post", "readback"],
        "passes": {
            "depth prepass": {"frames": 600, "mean": 0.6, "p50": 0.5, "p95": 0.9, "p99": 1.1, "max": 2.4},
            "opaque": {"frames": 600, "mean": 1.6, "p50": 1.5, "p95": 2.2, "p99": 2.6, "max": 5.0},
            "sky": {"frames": 600, "mean": 0.0, "p50": 0.0, "p95": 0.0, "p99": 0.0, "max": 0.0},
            "post": {"frames": 600, "mean": 0.9, "p50": 0.8, "p95": 1.2, "p99": 1.5, "max": 3.2},
            "readback": {"frames": 600, "mean": 0.3, "p50": 0.25, "p95": 0.4, "p99": 0.5, "max": 1.1},
        },
    }
    data.update(overrides)
    return data


class CompareTest(unittest.TestCase):
    """Both results written to a temporary directory and run through the
    tool's own main(), so the exit code and the output are the real ones."""

    def compare(self, before, after, *args):
        """Runs the tool on two results; returns (exit code, stdout)."""
        with tempfile.TemporaryDirectory() as directory:
            paths = []
            for name, data in (("before.json", before), ("after.json", after)):
                path = pathlib.Path(directory) / name
                path.write_text(json.dumps(data))
                paths.append(str(path))
            if isinstance(after, str):  # a path to write instead of a result
                paths[1] = after
            out = io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
                status = bench_compare.main([*paths, *args])
        return status, out.getvalue()

    @staticmethod
    def scripts(**overrides):
        """The scripts phase's numbers, a second's worth changed around."""
        numbers = {"frames": 600, "mean": 3.0, "p50": 2.5, "p95": 4.0, "p99": 5.0, "max": 9.0}
        numbers.update(overrides)
        return numbers

    @staticmethod
    def post(**overrides):
        """The post pass's numbers, a second's worth changed around."""
        numbers = {"frames": 600, "mean": 0.9, "p50": 0.8, "p95": 1.2, "p99": 1.5, "max": 3.2}
        numbers.update(overrides)
        return numbers

    def test_phase_over_threshold_regresses(self):
        """A phase that is well past the threshold exits 1 and names itself."""
        after = result()
        after["phases"]["scripts"] = self.scripts(mean=6.0, p50=5.6, p95=7.0, p99=8.0, max=12.0)
        status, out = self.compare(result(), after)
        self.assertEqual(status, 1)
        self.assertIn("scripts p50", out)
        self.assertIn("2 regression(s)", out)

    def test_phase_at_or_below_threshold_passes(self):
        """Exactly at the threshold is not a regression: the move has to be
        more than the threshold, and just under it certainly isn't."""
        after = result()
        after["phases"]["render"] = {"frames": 600, "mean": 4.4, "p50": 4.18,
                                     "p95": 5.5, "p99": 6.6, "max": 13.0}
        status, out = self.compare(result(), after)
        self.assertEqual(status, 0)
        self.assertIn("no regressions", out)

    def test_improvement_is_not_a_regression(self):
        """A phase that got faster is reported as an improvement, and the
        run still exits 0."""
        after = result()
        after["phases"]["scripts"] = self.scripts(mean=2.0, p50=2.0, p95=3.0, p99=3.0, max=6.0)
        status, out = self.compare(result(), after)
        self.assertEqual(status, 0)
        self.assertIn("improvements", out)
        self.assertIn("scripts p50", out)
        self.assertNotIn("regressions (", out)

    def test_missing_phase_is_reported_not_judged(self):
        """A phase only one run has is listed as such and never a regression,
        however slow it is."""
        after = result()
        after["phases"]["physics"] = self.scripts(mean=90.0, p50=90.0, p95=90.0, p99=90.0, max=90.0)
        status, out = self.compare(result(), after)
        self.assertEqual(status, 0)
        self.assertIn("after only:  physics", out)

        before = result()
        before["phases"]["physics"] = self.scripts(mean=90.0, p50=90.0, p95=90.0, p99=90.0, max=90.0)
        status, out = self.compare(before, result())
        self.assertEqual(status, 0)
        self.assertIn("before only: physics", out)

    def test_threshold_flag_changes_the_verdict(self):
        """The same pair of results is a regression at a tight threshold and
        nothing at a loose one."""
        after = result()
        after["phases"]["scripts"] = self.scripts(mean=3.9, p50=3.7, p95=4.5, p99=5.5, max=9.0)
        self.assertEqual(self.compare(result(), after, "--threshold", "50")[0], 0)
        status, out = self.compare(result(), after, "--threshold", "5")
        self.assertEqual(status, 1)
        self.assertIn("scripts p50", out)

    def test_window_estimate_is_compared_like_a_phase(self):
        """cpu and pipelined are phases like any other: a regression in either
        counts, and an improvement in either does not."""
        after = result()
        after["phases"]["pipelined"] = {"frames": 600, "mean": 8.0, "p50": 7.5, "p95": 10.0, "p99": 13.0, "max": 25.0}
        status, out = self.compare(result(), after)
        self.assertEqual(status, 1)
        self.assertIn("pipelined p50", out)

        faster = result()
        faster["phases"]["cpu"] = {"frames": 600, "mean": 3.0, "p50": 2.9, "p95": 4.0, "p99": 5.0, "max": 10.0}
        status, out = self.compare(result(), faster)
        self.assertEqual(status, 0)
        self.assertIn("cpu p50", out)
        self.assertIn("improvements", out)

    def test_under_the_floor_is_noise(self):
        """Two roundings of nothing are not a 100% regression: below the
        floor a phase is printed without a percentage and never judged."""
        after = result()
        after["phases"]["camera"] = {"frames": 600, "mean": 0.004, "p50": 0.003, "p95": 0.01, "p99": 0.02, "max": 0.03}
        status, out = self.compare(result(), after)
        self.assertEqual(status, 0)
        self.assertIn("no regressions", out)
        self.assertIn("camera", out)
        self.assertIn("noise", out)
        # The row keeps its numbers and has no percentage to print.
        self.assertIn("0.00 ms", out)
        self.assertIn("0.00 ms   0.00 ms   -      noise", out)
        self.assertNotIn("+300.0%", out)

        # With no floor the same pair is judged, and there is still no
        # percentage to print of a zero.
        status, out = self.compare(result(), after, "--floor", "0")
        self.assertEqual(status, 1)
        self.assertIn("camera p50 - (0.00 -> 0.00 ms)", out)

    def test_floor_flag_changes_the_verdict(self):
        """The same pair of results is judged at the default floor and not at
        a floor above both of the percentiles it is judged on."""
        after = result()
        after["phases"]["render"] = {"frames": 600, "mean": 6.0, "p50": 5.8, "p95": 7.0, "p99": 6.5, "max": 12.0}
        # +53% on p50 is over the default threshold, but both percentiles it
        # is judged on are under a floor of 7 ms, which is where it stops
        # counting.
        status, out = self.compare(result(), after, "--floor", "7")
        self.assertEqual(status, 0)
        self.assertIn("render p50", out)
        status, out = self.compare(result(), after, "--floor", "0.05")
        self.assertEqual(status, 1)
        self.assertIn("render p50", out)

    def test_a_phase_that_was_doing_nothing_is_judged(self):
        """The floor is a floor on the noise, not on the comparison: a phase
        that spent no time and now spends a millisecond has regressed, with no
        percentage to print of a zero."""
        after = result()
        after["phases"]["camera"] = {"frames": 600, "mean": 1.0, "p50": 0.9, "p95": 2.0, "p99": 3.0, "max": 4.0}
        status, out = self.compare(result(), after)
        self.assertEqual(status, 1)
        self.assertIn("camera p50", out)
        self.assertIn("camera p50 - (0.00 -> 0.90 ms)", out)

    def test_extras_regress_the_way_round_they_should(self):
        """Peak RSS and the load-in are lower-is-better like a phase, and the
        fps equivalent is higher-is-better, all on the same threshold."""
        after = result(peakRssMb=1000.0)
        status, out = self.compare(result(), after)
        self.assertEqual(status, 1)
        self.assertIn("peakRssMb", out)
        self.assertIn("[extra, lower is better]", out)

        # The same growth in fpsEquivalent is not a regression, and a fall is.
        after = result(fpsEquivalent=100.0)
        status, out = self.compare(result(), after)
        self.assertEqual(status, 1)
        self.assertIn("fpsEquivalent", out)
        self.assertIn("[extra, higher is better]", out)
        self.assertEqual(self.compare(result(), result(fpsEquivalent=140.0))[0], 0)

    def test_passes_are_a_table_of_their_own(self):
        """The passes are compared like the phases but reported apart: under
        their own heading between the phases and the extras, in the order the
        result names them."""
        status, out = self.compare(result(), result())
        self.assertEqual(status, 0)
        self.assertIn("passes (ms, lower is better; p50 and p99 decide, the rest are context)", out)
        self.assertLess(out.index("phases (ms,"), out.index("passes (ms,"))
        self.assertLess(out.index("passes (ms,"), out.index("extras (not phases"))
        self.assertLess(out.index("depth prepass p50"), out.index("opaque p50"))
        self.assertLess(out.index("opaque p50"), out.index("readback p50"))
        # The table's own header, so a pass is never read as a phase.
        self.assertIn("pass", out)

    def test_pass_over_threshold_regresses(self):
        """A pass that is well past the threshold exits 1 and names itself, the
        way a phase does, with no phase having moved at all."""
        after = result()
        after["passes"]["post"] = self.post(mean=2.4, p50=2.2, p95=2.8, p99=1.6, max=6.0)
        status, out = self.compare(result(), after)
        self.assertEqual(status, 1)
        self.assertIn("post p50", out)
        self.assertIn("post p50 +175.0% (0.80 -> 2.20 ms)", out)
        self.assertIn("1 regression(s)", out)

    def test_pass_improvement_is_not_a_regression(self):
        """A pass that got faster is reported as an improvement, and the run
        still exits 0."""
        after = result()
        after["passes"]["opaque"] = {"frames": 600, "mean": 1.0, "p50": 0.9, "p95": 1.2, "p99": 1.4, "max": 3.0}
        status, out = self.compare(result(), after)
        self.assertEqual(status, 0)
        self.assertIn("improvements", out)
        self.assertIn("opaque p50", out)
        self.assertNotIn("regressions (", out)

    def test_pass_under_the_floor_is_noise(self):
        """The floor is the floor on a pass too: below it the row keeps its
        numbers, has no percentage, and is never judged."""
        after = result()
        after["passes"]["sky"] = {"frames": 600, "mean": 0.004, "p50": 0.003, "p95": 0.01, "p99": 0.02, "max": 0.03}
        status, out = self.compare(result(), after)
        self.assertEqual(status, 0)
        self.assertIn("no regressions", out)
        self.assertIn("sky", out)
        self.assertIn("noise", out)
        self.assertIn("sky p50             0.00 ms  0.00 ms  -      noise", out)
        self.assertNotIn("sky p50 +", out)

        # With no floor the same pair is judged, and there is still no
        # percentage to print of a zero.
        status, out = self.compare(result(), after, "--floor", "0")
        self.assertEqual(status, 1)
        self.assertIn("sky p50 - (0.00 -> 0.00 ms)", out)

    def test_missing_pass_is_reported_not_judged(self):
        """A pass only one run has is listed as such and never a regression,
        however slow it is."""
        after = result()
        after["passes"]["water"] = self.post(mean=9.0, p50=9.0, p95=9.0, p99=9.0, max=9.0)
        status, out = self.compare(result(), after)
        self.assertEqual(status, 0)
        self.assertIn("pass after only:  water", out)

        before = result()
        before["passes"]["water"] = self.post(mean=9.0, p50=9.0, p95=9.0, p99=9.0, max=9.0)
        status, out = self.compare(before, result())
        self.assertEqual(status, 0)
        self.assertIn("pass before only: water", out)

    def test_threshold_and_floor_drive_the_pass_verdicts(self):
        """The same pair of passes is a regression at the default threshold and
        not at a loose one, and a floor above both of the percentiles it is
        judged on stops it counting."""
        after = result()
        after["passes"]["post"] = self.post(mean=1.1, p50=1.0, p95=1.3, p99=1.6, max=3.3)
        # +25% on p50 and +6.7% on p99: over the default threshold on one of
        # the two, under a 50% one on both.
        self.assertEqual(self.compare(result(), after)[0], 1)
        self.assertEqual(self.compare(result(), after, "--threshold", "50")[0], 0)
        status, out = self.compare(result(), after, "--floor", "2")
        self.assertEqual(status, 0)
        self.assertIn("post p50", out)

    def test_ray_tracing_counters_are_not_a_setting(self):
        """A result writes its ray tracing counters under the same name as the
        rayTracing flag, so a json parser sees the counters: they are not
        compared as a setting, and a run of them differing is not a warning."""
        before = result(rayTracing={"blasBuilds": 12, "tlasBuilds": 1, "instances": 250, "skip": "none"})
        after = result(rayTracing={"blasBuilds": 40, "tlasBuilds": 1, "instances": 251, "skip": "sun"})
        status, out = self.compare(before, after)
        self.assertEqual(status, 0)
        self.assertIn("rayTracing", out)
        self.assertNotIn("warning:", out)

    def test_bad_files_exit_two(self):
        """A missing file, a file that is not json and a file that is not a
        result are all usage errors, not regressions."""
        for contents in ("{not json", json.dumps({"hello": "world"}), json.dumps([1, 2, 3])):
            with self.subTest(contents=contents):
                with tempfile.TemporaryDirectory() as directory:
                    path = pathlib.Path(directory) / "after.json"
                    path.write_text(contents)
                    status, _ = self.compare(result(), str(path))
                self.assertEqual(status, 2)

        with tempfile.TemporaryDirectory() as directory:
            status, _ = self.compare(result(), str(pathlib.Path(directory) / "absent.json"))
        self.assertEqual(status, 2)

    def test_metadata_mismatch_warns_without_failing(self):
        """Two runs from different commits, on different hardware, are still
        compared -- with a warning, and with the same exit code as the
        numbers alone would give."""
        after = result(commit="e4f5a6b", gpu="AMD Radeon RX 7900 XTX", preset="average")
        after["phases"]["scripts"] = self.scripts(mean=6.0, p50=5.6, p95=7.0, p99=8.0, max=12.0)

        # A warning on its own changes nothing.
        status, out = self.compare(result(), result(commit="e4f5a6b"), "--threshold", "1000")
        self.assertEqual(status, 0)
        self.assertIn("warning:", out)
        self.assertIn("commit", out)
        self.assertIn("gpu", out)
        self.assertIn("differs", out)

        # With regressions in it, the metadata changes nothing either.
        status, out = self.compare(result(), after)
        self.assertEqual(status, 1)
        self.assertIn("2 regression(s)", out)
        self.assertIn("warning:", out)

    def test_phase_filter(self):
        """--phases looks at the phases it was given and leaves the others
        out; the extras are compared either way."""
        after = result()
        after["phases"]["scripts"] = self.scripts(mean=6.0, p50=5.6, p95=7.0, p99=8.0, max=12.0)
        status, out = self.compare(result(), after, "--phases", "render")
        self.assertEqual(status, 0)
        self.assertNotIn("scripts", out)
        self.assertIn("render p50", out)

    def test_identical_runs(self):
        """The same result twice changes nothing at all."""
        status, out = self.compare(result(), result())
        self.assertEqual(status, 0)
        self.assertIn("no regressions", out)
        self.assertNotIn("warning:", out)

    def test_width_and_height_count_as_the_resolution(self):
        """A build that writes the resolution as two numbers is compared on
        it all the same."""
        after = result(commit="e4f5a6b")
        del after["resolution"]
        after["width"], after["height"] = 1280, 720
        status, out = self.compare(result(), after)
        self.assertEqual(status, 0)
        self.assertIn("resolution", out)
        self.assertIn("differs", out)


if __name__ == "__main__":
    unittest.main()