import unittest

from lsp_client import FIXTURES, LspClient, run_cli


class DefinitionTests(unittest.TestCase):
	def test_priority_order_loads_dependent_files(self):
		client = LspClient(FIXTURES / "definitions")

		try:
			_, diagnostics = client.open("main.luau")
			self.assertEqual(diagnostics, [])

			listed = client.request("lunaut/definitions", {})[0]["definitions"]
			names = [definition["name"] for definition in listed]
			self.assertEqual(names[:2], ["@base", "@extra"])
			self.assertIn("@misc", names)
			self.assertTrue(all(definition["success"] for definition in listed), listed)

			priorities = {definition["name"]: definition["priority"] for definition in listed}
			self.assertEqual(priorities["@base"], 100)
			self.assertEqual(priorities["@extra"], 50)
		finally:
			client.close()

	def test_wrong_priority_fails(self):
		client = LspClient(FIXTURES / "definitions-reversed")

		try:
			listed = client.request("lunaut/definitions", {})[0]["definitions"]
			extra = next(definition for definition in listed if definition["name"] == "@extra")
			self.assertFalse(extra["success"])
			self.assertEqual(listed[0]["name"], "@extra")
		finally:
			client.close()

	def test_cli_duplicate_keeps_config_priority(self):
		fixture = FIXTURES / "definitions"
		result = run_cli(
			"analyze",
			"main.luau",
			"--definition=@base=types/base.d.luau#500",
			"--verbose",
			cwd=fixture,
		)
		self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
		self.assertIn("definitions @base (priority 100", result.stderr)


if __name__ == "__main__":
	unittest.main()
