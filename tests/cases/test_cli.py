import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

from lsp_client import FIXTURES, find_binary, run_cli


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

	def test_update_check_compares_versions(self):
		newer = run_cli("update", "--check", "99.0.0")
		self.assertEqual(newer.returncode, 0)
		self.assertIn("lunaut 99.0.0 is available", newer.stdout)

		older = run_cli("update", "--check", "v0.0.1")
		self.assertEqual(older.returncode, 0)
		self.assertIn("is up to date", older.stdout)

	def test_update_dry_run_targets_this_binary(self):
		result = run_cli("update", "--dry-run", "0.1.0")
		self.assertEqual(result.returncode, 0, result.stderr)
		self.assertIn("to 0.1.0 in", result.stdout)
		self.assertIn("LUNAUT_VERSION=0.1.0", result.stdout)
		self.assertIn("LUNAUT_EDITORS=path", result.stdout)
		self.assertIn("LUNAUT_NO_PATH=1", result.stdout)
		self.assertIn("LUNAUT_INSTALL_DIR=" + str(find_binary().parent), result.stdout)
		self.assertIn("installers/install", result.stdout)

	def test_update_respects_offline_mode(self):
		environment = dict(os.environ, LUNAUT_OFFLINE="1")
		result = subprocess.run([str(find_binary()), "update", "--check"], capture_output=True, text=True, env=environment, timeout=60)
		self.assertEqual(result.returncode, 1)
		self.assertIn("LUNAUT_OFFLINE", result.stderr)

	def test_update_leaves_editor_copies_alone(self):
		with tempfile.TemporaryDirectory() as folder:
			copy = pathlib.Path(folder) / "extensions" / "theexe.lunaut-luau" / "bin" / find_binary().name
			copy.parent.mkdir(parents=True)
			shutil.copy2(find_binary(), copy)
			result = subprocess.run([str(copy), "update", "--dry-run", "0.1.0"], capture_output=True, text=True, timeout=60)
			self.assertEqual(result.returncode, 1)
			self.assertIn("editor extension", result.stderr)

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
