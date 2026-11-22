import unittest

from lsp_client import FIXTURES, run_cli


class CliTests(unittest.TestCase):
	def test_version(self):
		result = run_cli("version")
		self.assertEqual(result.returncode, 0)
		self.assertRegex(result.stdout, r"^lunaut \d+\.\d+\.\d+ \(Luau \d+\.\d+")

	def test_help(self):
		result = run_cli("--help")
		self.assertEqual(result.returncode, 0)
		self.assertIn("lunaut analyze", result.stdout)
		self.assertIn("--definition=", result.stdout)
		self.assertIn("lunaut update", result.stdout)
		self.assertIn("--dry-run", result.stdout)

	def test_update_dry_run(self):
		result = run_cli("update", "--dry-run")
		self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
		self.assertIn("https://raw.githubusercontent.com/TheExeDEV/lunaut/main/installers/install", result.stdout)
		self.assertIn("LUNAUT_EDITORS", result.stdout)

	def test_update_offline(self):
		result = run_cli("update")
		self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
		self.assertIn("offline mode", result.stderr)

	def test_unknown_option(self):
		result = run_cli("--what")
		self.assertEqual(result.returncode, 2)
		self.assertIn("unknown option", result.stderr)

	def test_analyze_reports_errors(self):
		result = run_cli("analyze", "main.luau", cwd=FIXTURES / "basic")
		self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
		self.assertIn("main.luau:4:", result.stdout)
		self.assertIn("[TypeError]", result.stdout)
		self.assertIn("FunctionUnused", result.stdout)

	def test_analyze_clean_file(self):
		result = run_cli("analyze", "util.luau", "alias.luau", cwd=FIXTURES / "basic")
		self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
		self.assertIn("2 files, 0 errors", result.stdout)

	def test_analyze_unknown_require(self):
		result = run_cli("analyze", "broken.luau", cwd=FIXTURES / "basic")
		self.assertEqual(result.returncode, 1)
		self.assertIn("does_not_exist", result.stdout)


if __name__ == "__main__":
	unittest.main()
