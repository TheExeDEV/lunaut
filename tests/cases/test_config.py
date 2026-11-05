import unittest

from lsp_client import FIXTURES, LspClient, position_of


class ConfigTests(unittest.TestCase):
	def test_config_luau_aliases(self):
		client = LspClient(FIXTURES / "config-luau")

		try:
			uri, diagnostics = client.open("main.luau")
			self.assertEqual(diagnostics, [])

			text = (FIXTURES / "config-luau" / "main.luau").read_text(encoding="utf-8")
			line, character = position_of(text, "limit", 1)
			hover = client.at("textDocument/hover", uri, line, character)
			self.assertIn("const limit: number", hover["contents"]["value"])
		finally:
			client.close()

	def test_base_luaurc_and_lua_files(self):
		client = LspClient(FIXTURES / "base-luaurc")

		try:
			_, diagnostics = client.open("main.luau")
			self.assertEqual(diagnostics, [])

			_, diagnostics = client.open("legacy.lua")
			self.assertEqual(diagnostics, [])
		finally:
			client.close()

	def test_require_path_completion(self):
		client = LspClient(FIXTURES / "basic")

		try:
			text = 'local a = require("@")\nlocal b = require("./lib/")\n'
			uri, _ = client.open("requires.luau", text)

			aliases = client.at("textDocument/completion", uri, 0, len('local a = require("@'))["items"]
			labels = {item["label"] for item in aliases}
			self.assertIn("@lib", labels)
			self.assertIn("./", labels)

			files = client.at("textDocument/completion", uri, 1, len('local b = require("./lib/'))["items"]
			edits = {item["textEdit"]["newText"] for item in files if "textEdit" in item}
			self.assertIn("./lib/math2", edits)
		finally:
			client.close()


if __name__ == "__main__":
	unittest.main()
