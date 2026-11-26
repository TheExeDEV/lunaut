import pathlib
import shutil
import tempfile
import unittest

from lsp_client import run_cli

SOLVERS = ("false", "true")


class FileDirectiveTests(unittest.TestCase):
	def setUp(self):
		self.folder = pathlib.Path(tempfile.mkdtemp())
		self.addCleanup(shutil.rmtree, self.folder, True)
		self.write(".luaurc", '{"languageMode": "strict", "lint": {"*": true}}')

	def write(self, name, text):
		(self.folder / name).write_text(text, encoding="utf-8")

	def analyze(self, solver, *files):
		return run_cli("analyze", f"--flag:LuauSolverV2={solver}", *files, cwd=self.folder).stdout

	def test_nocheck_module_returns_any(self):
		self.write("checked_off.luau", "--!nocheck\nreturn { value = 5 }\n")
		self.write("user.luau", '--!strict\nlocal module = require("./checked_off")\nlocal text: string = module.value\nreturn text\n')

		for solver in SOLVERS:
			output = self.analyze(solver, "user.luau")
			self.assertIn("0 errors", output, f"solver {solver}: {output}")

	def test_ignore_mode_wins_over_the_usual_directive(self):
		self.write("studio.luau", "--!nocheck\n--!ignore strict\nlocal value: number = \"text\"\nreturn value\n")
		self.write("plain.luau", "--!nocheck\nlocal value: number = \"text\"\nreturn value\n")

		for solver in SOLVERS:
			output = self.analyze(solver, "studio.luau", "plain.luau")
			self.assertIn("studio.luau:3:", output, f"solver {solver}: {output}")
			self.assertNotIn("plain.luau:3:", output, f"solver {solver}: {output}")

	def test_ignore_values_add_up(self):
		self.write("both.luau", "--!ignore nocheck\n--!ignore nolint\nlocal unused = 1\nlocal value: number = \"text\"\nreturn value\n")
		self.write("lint.luau", '--!ignore "nolint"\nlocal unused = 1\nreturn nil\n')
		self.write("user.luau", '--!strict\nlocal module = require("./both")\nlocal text: string = module\nreturn text\n')

		for solver in SOLVERS:
			output = self.analyze(solver, "both.luau", "lint.luau", "user.luau")
			self.assertIn("3 files, 0 errors, 0 warnings", output, f"solver {solver}: {output}")
			self.assertNotIn("Unknown comment directive", output)

	def test_noerror_hides_everything(self):
		self.write("quiet.luau", "--!noerror\nlocal unused = 1\nlocal value: number = \"text\"\nreturn value\n")

		for solver in SOLVERS:
			output = self.analyze(solver, "quiet.luau")
			self.assertIn("1 files, 0 errors, 0 warnings", output, f"solver {solver}: {output}")

	def test_unknown_ignore_value_is_reported(self):
		self.write("typo.luau", "--!ignore stict\nreturn nil\n")
		output = self.analyze("false", "typo.luau")
		self.assertIn("Unknown --!ignore value", output)
		self.assertNotIn("Unknown comment directive", output)


if __name__ == "__main__":
	unittest.main()
