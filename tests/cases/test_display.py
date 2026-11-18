import unittest

from lsp_client import FIXTURES, LspClient, position_of


class RefinedDisplayTests(unittest.TestCase):
	@classmethod
	def setUpClass(cls):
		cls.client = LspClient(FIXTURES / "refine-display")
		cls.text = (FIXTURES / "refine-display" / "main.luau").read_text(encoding="utf-8")
		cls.uri, cls.diagnostics = cls.client.open("main.luau")

	@classmethod
	def tearDownClass(cls):
		cls.client.close()

	def hover(self, needle, offset):
		line, character = position_of(self.text, needle, offset)
		return self.client.at("textDocument/hover", self.uri, line, character)["contents"]["value"].splitlines()[1]

	def test_no_diagnostics(self):
		self.assertEqual(self.diagnostics, [])

	def test_refined_local_keeps_declared_type(self):
		self.assertEqual(self.hover("Direction =", 1), "const Direction: direction")

	def test_refined_uses_have_one_token_type(self):
		tokens = self.client.request("textDocument/semanticTokens/full", {"textDocument": {"uri": self.uri}})["data"]
		legend = self.client.capabilities["semanticTokensProvider"]["legend"]["tokenTypes"]
		lines = self.text.split("\n")
		kinds = set()
		line = 0
		column = 0

		for index in range(0, len(tokens), 5):
			delta_line, delta_column, length, kind, _ = tokens[index : index + 5]
			line += delta_line
			column = column + delta_column if delta_line == 0 else delta_column

			if lines[line][column : column + length] == "Direction":
				kinds.add(legend[kind])

		self.assertEqual(kinds, {"variable"})

	def test_nested_table_is_not_named_after_its_alias(self):
		self.assertEqual(self.hover("empty()", 1), "function empty(): {}")


if __name__ == "__main__":
	unittest.main()
