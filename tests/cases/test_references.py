import unittest

from lsp_client import FIXTURES, LspClient, LspError, position_of


def summarize(locations):
	return sorted((location["uri"].rsplit("/", 1)[1], location["range"]["start"]["line"]) for location in locations)


def summarize_edits(edit):
	return {uri.rsplit("/", 1)[1]: sorted(change["range"]["start"]["line"] for change in changes) for uri, changes in edit["changes"].items()}


class ReferenceTests(unittest.TestCase):
	options = {}

	@classmethod
	def setUpClass(cls):
		cls.client = LspClient(FIXTURES / "basic", cls.options)
		cls.main = (FIXTURES / "basic" / "main.luau").read_text(encoding="utf-8")
		cls.config = (FIXTURES / "basic" / "config.luau").read_text(encoding="utf-8")
		cls.consumer = (FIXTURES / "basic" / "consumer.luau").read_text(encoding="utf-8")
		cls.main_uri, _ = cls.client.open("main.luau")
		cls.config_uri, _ = cls.client.open("config.luau")

	@classmethod
	def tearDownClass(cls):
		cls.client.close()

	def rename(self, uri, line, character, name):
		return self.client.request(
			"textDocument/rename",
			{"textDocument": {"uri": uri}, "position": {"line": line, "character": character}, "newName": name},
		)

	def test_local_references(self):
		line, character = position_of(self.main, "count", 1)
		references = self.client.at("textDocument/references", self.main_uri, line, character)
		self.assertEqual(summarize(references), [("main.luau", 2), ("main.luau", 3)])

	def test_references_without_declaration(self):
		line, character = position_of(self.main, "count", 1)
		references = self.client.request(
			"textDocument/references",
			{
				"textDocument": {"uri": self.main_uri},
				"position": {"line": line, "character": character},
				"context": {"includeDeclaration": False},
			},
		)
		self.assertEqual(summarize(references), [("main.luau", 3)])

	def test_property_references_across_files(self):
		line, character = position_of(self.main, "util.add", 6)
		references = self.client.at("textDocument/references", self.main_uri, line, character)
		self.assertEqual(summarize(references), [("main.luau", 2), ("util.luau", 3)])

	def test_references_from_table_key_find_unopened_files(self):
		line, character = position_of(self.config, "maxPlayers", 2)
		references = self.client.at("textDocument/references", self.config_uri, line, character)
		self.assertEqual(summarize(references), [("config.luau", 2), ("consumer.luau", 2)])

	def test_prepare_rename(self):
		line, character = position_of(self.main, "count", 1)
		prepared = self.client.at("textDocument/prepareRename", self.main_uri, line, character)
		self.assertEqual(prepared["placeholder"], "count")

		line, character = position_of(self.main, "print", 1)
		self.assertIsNone(self.client.at("textDocument/prepareRename", self.main_uri, line, character))

	def test_rename_local(self):
		line, character = position_of(self.main, "count", 1)
		self.assertEqual(summarize_edits(self.rename(self.main_uri, line, character, "total")), {"main.luau": [2, 3]})

	def test_rename_module_function(self):
		line, character = position_of(self.main, "util.greet", 6)
		edits = summarize_edits(self.rename(self.main_uri, line, character, "welcome"))
		self.assertEqual(edits, {"main.luau": [4], "util.luau": [7]})

	def test_rename_table_key(self):
		line, character = position_of(self.config, "maxPlayers", 2)
		edits = summarize_edits(self.rename(self.config_uri, line, character, "playerLimit"))
		self.assertEqual(edits, {"config.luau": [2], "consumer.luau": [2]})

	def test_rename_rejects_bad_names(self):
		line, character = position_of(self.main, "count", 1)

		for name in ("end", "1abc", "has space", ""):
			with self.assertRaises(LspError):
				self.rename(self.main_uri, line, character, name)


class NewSolverReferenceTests(ReferenceTests):
	options = {"fflags": {"sync": False, "enable_new_solver": True}}


if __name__ == "__main__":
	unittest.main()
