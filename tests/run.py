import argparse
import datetime
import os
import pathlib
import sys
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))

from lsp_client import find_binary, run_cli


class RecordingResult(unittest.TextTestResult):
	def __init__(self, *args, **kwargs):
		super().__init__(*args, **kwargs)
		self.records = []
		self.started = 0.0

	def startTest(self, test):
		self.started = time.time()
		super().startTest(test)

	def record(self, test, status, details=""):
		self.records.append((test.id(), status, time.time() - self.started, details))

	def addSuccess(self, test):
		super().addSuccess(test)
		self.record(test, "PASS")

	def addFailure(self, test, error):
		super().addFailure(test, error)
		self.record(test, "FAIL", self._exc_info_to_string(error, test))

	def addError(self, test, error):
		super().addError(test, error)
		self.record(test, "ERROR", self._exc_info_to_string(error, test))

	def addSkip(self, test, reason):
		super().addSkip(test, reason)
		self.record(test, "SKIP", reason)


def write_report(path, result, elapsed):
	version = run_cli("version").stdout.strip()
	passed = sum(1 for record in result.records if record[1] == "PASS")
	lines = [
		"# Test report",
		"",
		f"- Date: {datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')}",
		f"- Binary: `{find_binary()}`",
		f"- Version: `{version}`",
		f"- Result: **{passed}/{len(result.records)} passed** in {elapsed:.1f}s",
		"",
		"| Test | Status | Time |",
		"| --- | --- | --- |",
	]

	for name, status, duration, _ in result.records:
		lines.append(f"| `{name.replace('cases.', '')}` | {status} | {duration:.2f}s |")

	failures = [record for record in result.records if record[1] in ("FAIL", "ERROR")]

	if failures:
		lines += ["", "## Failures", ""]

		for name, status, _, details in failures:
			lines += [f"### {name}", "", "```", details.strip(), "```", ""]

	path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
	parser = argparse.ArgumentParser(description="Run every lunaut test and write tests/report.md")
	parser.add_argument("pattern", nargs="?", default="test_*.py")
	parser.add_argument("--binary")
	parser.add_argument("--report", default=str(ROOT / "report.md"))
	arguments = parser.parse_args()

	if arguments.binary:
		os.environ["LUNAUT_BINARY"] = arguments.binary

	suite = unittest.defaultTestLoader.discover(str(ROOT / "cases"), pattern=arguments.pattern, top_level_dir=str(ROOT))
	runner = unittest.TextTestRunner(verbosity=2, resultclass=RecordingResult)
	started = time.time()
	result = runner.run(suite)
	write_report(pathlib.Path(arguments.report), result, time.time() - started)
	print(f"\nreport: {arguments.report}")
	return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
	sys.exit(main())
