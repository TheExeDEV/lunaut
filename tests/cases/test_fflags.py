import json
import unittest

from lsp_client import FIXTURES, LspClient, run_cli


class FFlagTests(unittest.TestCase):
	def test_flags_enabled_by_default(self):
		result = run_cli("flags")
		self.assertEqual(result.returncode, 0)
		self.assertIn("(bool, EnableByDefault)", result.stdout)

	def test_no_flags_enabled(self):
		result = run_cli("flags", "--no-flags-enabled")
		self.assertEqual(result.returncode, 0)
		self.assertNotIn("EnableByDefault", result.stdout)

	def test_cli_override(self):
		result = run_cli("flags", "--flag:LuauSolverV2=false", "--flag:LuauTypeInferIterationLimit=1234")
		self.assertIn("LuauSolverV2=false  (bool, Cli)", result.stdout)
		self.assertIn("LuauTypeInferIterationLimit=1234  (int, Cli)", result.stdout)

	def test_json_output(self):
		result = run_cli("flags", "--json")
		flags = json.loads(result.stdout)
		names = {flag["name"]: flag for flag in flags}
		self.assertIn("LuauSolverV2", names)
		self.assertEqual(names["LuauSolverV2"]["kind"], "bool")

	def test_invalid_value(self):
		result = run_cli("flags", "--flag:LuauSolverV2=maybe")
		self.assertIn("expected boolean", result.stderr)

	def test_unknown_flag(self):
		result = run_cli("flags", "--flag:LuauDefinitelyNotAFlag=true")
		self.assertIn("unknown fflag: LuauDefinitelyNotAFlag", result.stderr)

	def test_settings_override_over_lsp(self):
		client = LspClient(
			FIXTURES / "basic",
			{"fflags": {"sync": False, "override": {"LuauTypeInferIterationLimit": "4321"}}},
		)

		try:
			flags = client.request("lunaut/fflags", {"filter": "LuauTypeInferIterationLimit"})
			self.assertEqual(flags[0]["value"], "4321")
			self.assertEqual(flags[0]["source"], "Override")
		finally:
			self.assertEqual(client.close(), 0)


if __name__ == "__main__":
	unittest.main()
