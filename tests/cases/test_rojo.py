import unittest

from lsp_client import FIXTURES, LspClient, position_of, run_cli


class RojoTests(unittest.TestCase):
	@classmethod
	def setUpClass(cls):
		cls.client = LspClient(FIXTURES / "rojo")
		cls.path = "src/server/Main.server.luau"
		cls.text = (FIXTURES / "rojo" / cls.path).read_text(encoding="utf-8")
		cls.uri, cls.diagnostics = cls.client.open(cls.path)

	@classmethod
	def tearDownClass(cls):
		cls.client.close()

	def test_status(self):
		workspace = self.client.request("lunaut/status", {})["workspaces"][0]
		self.assertEqual(workspace["platform"], "roblox")
		self.assertTrue(workspace["sourcemap"]["loaded"])
		self.assertGreaterEqual(workspace["sourcemap"]["nodes"], 7)

	def test_instance_requires_resolve(self):
		self.assertEqual(self.diagnostics, [])

	def test_required_types_flow(self):
		broken = self.text.replace("local total: number", "local total: string")
		diagnostics = self.client.change(self.uri, broken)
		self.assertTrue(any(diagnostic["code"] == "TypeError" for diagnostic in diagnostics), diagnostics)
		self.assertEqual(self.client.change(self.uri, self.text, version=3), [])

	def test_hover_on_required_module(self):
		line, character = position_of(self.text, "Util.double", 6)
		hover = self.client.at("textDocument/hover", self.uri, line, character)
		self.assertIn("double(value: number): number", hover["contents"]["value"])

	def test_script_instance_type(self):
		line, character = position_of(self.text, "script.Parent", 2)
		hover = self.client.at("textDocument/hover", self.uri, line, character)
		self.assertIn("script: Script", hover["contents"]["value"])

	def test_completion_from_sourcemap_children(self):
		text = "local parent = script.Parent.\nlocal shared = game:GetService(\"ReplicatedStorage\").Shared.\n"
		self.client.change(self.uri, text, version=10)

		try:
			first = {
				item["label"] for item in self.client.at("textDocument/completion", self.uri, 0, len("local parent = script.Parent."))["items"]
			}
			second = {
				item["label"]
				for item in self.client.at(
					"textDocument/completion", self.uri, 1, len("local shared = game:GetService(\"ReplicatedStorage\").Shared.")
				)["items"]
			}
		finally:
			self.client.change(self.uri, self.text, version=11)

		self.assertTrue({"Helper", "Main"} <= first, first)
		self.assertIn("Util", second)

	def test_roblox_magic_functions(self):
		_, diagnostics = self.client.open("src/server/Magic.server.luau")
		self.assertEqual(diagnostics, [])

		_, diagnostics = self.client.open("src/server/BadMagic.server.luau")
		self.assertEqual(len(diagnostics), 1, diagnostics)
		self.assertIn("Invalid class name 'Nope'", diagnostics[0]["message"])

	def test_roblox_magic_new_solver(self):
		files = ["src/server/Magic.server.luau", "src/server/BadMagic.server.luau"]
		result = run_cli("analyze", *files, "--flag:LuauSolverV2=true", cwd=FIXTURES / "rojo")
		self.assertIn("2 files, 1 errors", result.stdout, result.stdout + result.stderr)
		self.assertIn("Invalid class name 'Nope'", result.stdout)

	def test_definitions_loaded(self):
		listed = self.client.request("lunaut/definitions", {})[0]["definitions"]
		self.assertTrue(all(definition["success"] for definition in listed), listed)

	def test_definition_into_sourcemap_module(self):
		line, character = position_of(self.text, "Util.double", 6)
		location = self.client.at("textDocument/definition", self.uri, line, character)
		self.assertTrue(location["uri"].endswith("Util.luau"), location)


if __name__ == "__main__":
	unittest.main()
