import unittest

from lsp_client import FIXTURES, LspClient, LspError, position_of


class LspTests(unittest.TestCase):
	@classmethod
	def setUpClass(cls):
		cls.client = LspClient(FIXTURES / "basic")
		cls.main_text = (FIXTURES / "basic" / "main.luau").read_text(encoding="utf-8")
		cls.main_uri, cls.main_diagnostics = cls.client.open("main.luau")

	@classmethod
	def tearDownClass(cls):
		cls.exit_code = cls.client.close()

	def test_capabilities(self):
		capabilities = self.client.capabilities
		self.assertTrue(capabilities["hoverProvider"])
		self.assertTrue(capabilities["definitionProvider"])
		self.assertIn(".", capabilities["completionProvider"]["triggerCharacters"])

	def test_diagnostics(self):
		codes = [diagnostic["code"] for diagnostic in self.main_diagnostics]
		self.assertIn("TypeError", codes)
		self.assertIn("FunctionUnused", codes)

		error = next(diagnostic for diagnostic in self.main_diagnostics if diagnostic["code"] == "TypeError")
		self.assertEqual(error["range"]["start"]["line"], 3)
		self.assertEqual(error["severity"], 1)

	def test_diagnostics_update_on_change(self):
		uri, _ = self.client.open("util.luau")
		fixed = (FIXTURES / "basic" / "util.luau").read_text(encoding="utf-8").replace("return a + b", "return a + true")
		diagnostics = self.client.change(uri, fixed)
		self.assertTrue(any(diagnostic["code"] == "TypeError" for diagnostic in diagnostics))

		original = (FIXTURES / "basic" / "util.luau").read_text(encoding="utf-8")
		self.assertEqual(self.client.change(uri, original, version=3), [])

	def test_hover_local(self):
		line, character = position_of(self.main_text, "count", 1)
		hover = self.client.at("textDocument/hover", self.main_uri, line, character)
		self.assertIn("count: number", hover["contents"]["value"])

	def test_hover_function(self):
		line, character = position_of(self.main_text, "util.add", 6)
		hover = self.client.at("textDocument/hover", self.main_uri, line, character)
		self.assertIn("function add(a: number, b: number): number", hover["contents"]["value"])

	def test_completion_members(self):
		text = self.main_text.replace("print(util.greet", "print(util.")
		uri, _ = self.client.open("completion.luau", text)
		line, character = position_of(text, "print(util.", len("print(util."))
		completion = self.client.at("textDocument/completion", uri, line, character)
		labels = {item["label"] for item in completion["items"]}
		self.assertTrue({"add", "greet"} <= labels, labels)

	def test_definition_local(self):
		line, character = position_of(self.main_text, "= count", 3)
		location = self.client.at("textDocument/definition", self.main_uri, line, character)
		self.assertEqual(location["range"]["start"]["line"], 2)

	def test_definition_cross_module(self):
		line, character = position_of(self.main_text, "util.add", 6)
		location = self.client.at("textDocument/definition", self.main_uri, line, character)
		self.assertTrue(location["uri"].lower().endswith("util.luau"), location)
		self.assertEqual(location["range"]["start"]["line"], 3)

	def test_definition_require(self):
		line, character = position_of(self.main_text, 'require("./util")', 3)
		location = self.client.at("textDocument/definition", self.main_uri, line, character)
		self.assertTrue(location["uri"].lower().endswith("util.luau"), location)

	def test_document_symbols(self):
		uri, _ = self.client.open("util.luau")
		symbols = self.client.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
		names = {symbol["name"] for symbol in symbols}
		self.assertTrue({"util", "util.add", "util.greet"} <= names, names)

	def test_signature_help(self):
		text = 'local util = require("./util")\nutil.add(1, '
		uri, _ = self.client.open("signature.luau", text)
		help = self.client.at("textDocument/signatureHelp", uri, 1, len("util.add(1, "))
		self.assertIn("add(a: number, b: number): number", help["signatures"][0]["label"])
		self.assertEqual(help["activeParameter"], 1)

	def test_alias_require(self):
		_, diagnostics = self.client.open("alias.luau")
		self.assertEqual(diagnostics, [])

	def test_status(self):
		status = self.client.request("lunaut/status", {})
		self.assertEqual(status["name"], "lunaut")
		self.assertEqual(status["workspaces"][0]["platform"], "standard")

	def test_unknown_method(self):
		with self.assertRaises(LspError):
			self.client.request("lunaut/nope", {})


if __name__ == "__main__":
	unittest.main()
