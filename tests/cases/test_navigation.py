import json
import os
import pathlib
import sys
import tempfile
import unittest

from lsp_client import FIXTURES, LspClient, LspError, position_of, to_key


def make_fake_stylua(directory):
	script = pathlib.Path(directory) / "fake_stylua.py"
	script.write_text(
		"import sys\n"
		"source = sys.stdin.read()\n"
		"prefix = '-- range\\n' if '--range-start' in sys.argv else ''\n"
		"sys.stdout.buffer.write((prefix + source.replace('x=1', 'x = 1')).encode('utf-8'))\n",
		encoding="utf-8",
	)

	if os.name == "nt":
		wrapper = pathlib.Path(directory) / "fake-stylua.cmd"
		wrapper.write_text(f'@"{sys.executable}" "{script}" %*\r\n', encoding="utf-8")
	else:
		wrapper = pathlib.Path(directory) / "fake-stylua"
		wrapper.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{script}" "$@"\n', encoding="utf-8")
		wrapper.chmod(0o755)

	return str(wrapper)


def code_actions(client, uri, line, character, diagnostics=()):
	return client.request(
		"textDocument/codeAction",
		{
			"textDocument": {"uri": uri},
			"range": {"start": {"line": line, "character": character}, "end": {"line": line, "character": character}},
			"context": {"diagnostics": list(diagnostics)},
		},
	)


class NavigationTests(unittest.TestCase):
	@classmethod
	def setUpClass(cls):
		cls.tools = tempfile.TemporaryDirectory()
		cls.client = LspClient(FIXTURES / "basic", {"fflags": {"sync": False}, "features": {"stylua": make_fake_stylua(cls.tools.name)}})
		cls.main = (FIXTURES / "basic" / "main.luau").read_text(encoding="utf-8")
		cls.main_uri, _ = cls.client.open("main.luau")
		cls.hints = (FIXTURES / "basic" / "hints.luau").read_text(encoding="utf-8")
		cls.hints_uri, _ = cls.client.open("hints.luau")

	@classmethod
	def tearDownClass(cls):
		cls.client.close()
		cls.tools.cleanup()

	def test_document_highlight(self):
		line, character = position_of(self.main, "count", 1)
		highlights = self.client.at("textDocument/documentHighlight", self.main_uri, line, character)
		self.assertEqual([(item["range"]["start"]["line"], item["kind"]) for item in highlights], [(2, 3), (3, 2)])

	def test_document_links(self):
		links = self.client.request("textDocument/documentLink", {"textDocument": {"uri": self.main_uri}})
		self.assertEqual(len(links), 1)
		self.assertTrue(links[0]["target"].endswith("util.luau"))
		self.assertEqual(links[0]["range"]["start"], {"line": 1, "character": 21})

	def test_workspace_symbols(self):
		symbols = self.client.request("workspace/symbol", {"query": "gre"})
		found = {(symbol["name"], symbol["containerName"]) for symbol in symbols}
		self.assertIn(("util.greet", "util.luau"), found)
		self.assertIn(("greet", "documented.luau"), found)

	def test_call_hierarchy(self):
		line, character = position_of(self.hints, "scale", 1)
		items = self.client.at("textDocument/prepareCallHierarchy", self.hints_uri, line, character)
		self.assertEqual(items[0]["name"], "scale")

		incoming = self.client.request("callHierarchy/incomingCalls", {"item": items[0]})
		self.assertEqual(incoming[0]["fromRanges"][0]["start"], {"line": 5, "character": 15})

		line, character = position_of(self.main, "util.add", 6)
		items = self.client.at("textDocument/prepareCallHierarchy", self.main_uri, line, character)
		self.assertEqual(items[0]["name"], "util.add")
		self.assertTrue(items[0]["uri"].endswith("util.luau"))

		incoming = self.client.request("callHierarchy/incomingCalls", {"item": items[0]})
		self.assertTrue(any(call["from"]["uri"].endswith("main.luau") for call in incoming), incoming)

	def test_outgoing_calls(self):
		uri, _ = self.client.open(
			"outgoing.luau",
			"local function helper()\n\treturn 1\nend\nlocal function run()\n\treturn helper() + helper()\nend\nprint(run())\n",
		)
		items = self.client.at("textDocument/prepareCallHierarchy", uri, 3, 16)
		outgoing = self.client.request("callHierarchy/outgoingCalls", {"item": items[0]})
		self.assertEqual(outgoing[0]["to"]["name"], "helper")
		self.assertEqual(len(outgoing[0]["fromRanges"]), 2)

	def test_autocomplete_end(self):
		uri, _ = self.client.open("ending.luau", "local function test()\n\t\n")
		self.client.request(
			"textDocument/completion",
			{"textDocument": {"uri": uri}, "position": {"line": 1, "character": 1}, "context": {"triggerKind": 2, "triggerCharacter": "\n"}},
		)
		edit = self.client.wait_for("workspace/applyEdit", timeout=30)["edit"]
		changes = next(iter(edit["changes"].values()))
		self.assertEqual(changes[0]["newText"], "end\n")

	def test_convert_quotes(self):
		uri, _ = self.client.open("quotes.luau", 'local s = "hello {"\n')
		edits = self.client.request(
			"textDocument/onTypeFormatting",
			{"textDocument": {"uri": uri}, "position": {"line": 0, "character": 17}, "ch": "{", "options": {"tabSize": 4, "insertSpaces": False}},
		)
		self.assertEqual([edit["range"]["start"]["character"] for edit in edits], [10, 18])
		self.assertTrue(all(edit["newText"] == "`" for edit in edits))

	def test_formatting_with_stylua(self):
		uri, _ = self.client.open("format.luau", "local x=1\nprint(x)\n")
		edits = self.client.request("textDocument/formatting", {"textDocument": {"uri": uri}, "options": {"tabSize": 4, "insertSpaces": False}})
		self.assertEqual(edits[0]["newText"], "local x = 1\nprint(x)\n")

		ranged = self.client.request(
			"textDocument/rangeFormatting",
			{
				"textDocument": {"uri": uri},
				"range": {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 9}},
				"options": {"tabSize": 4, "insertSpaces": False},
			},
		)
		self.assertTrue(ranged[0]["newText"].startswith("-- range\n"))

	def test_formatting_without_stylua(self):
		client = LspClient(FIXTURES / "basic", {"fflags": {"sync": False}, "features": {"stylua": "definitely-not-stylua"}})

		try:
			uri, _ = client.open("format.luau", "local x=1\n")
			with self.assertRaises(LspError) as raised:
				client.request("textDocument/formatting", {"textDocument": {"uri": uri}, "options": {"tabSize": 4, "insertSpaces": False}})
			self.assertIn("StyLua", raised.exception.args[0]["message"])
		finally:
			client.close()

	def test_sort_imports_and_annotation(self):
		text = 'local util = require("./util")\nlocal config = require("./config")\nlocal total = util.add(1, 2)\nprint(config, total)\n'
		uri, _ = self.client.open("sorting.luau", text)
		actions = {action["title"]: action for action in code_actions(self.client, uri, 2, 7)}

		sort = actions["Sort imports"]
		self.assertEqual(sort["kind"], "source.organizeImports")
		self.assertEqual(sort["edit"]["changes"][uri][0]["newText"], 'local config = require("./config")\nlocal util = require("./util")\n')

		annotation = actions["Add type annotation ': number'"]
		self.assertEqual(annotation["edit"]["changes"][uri][0]["range"]["start"], {"line": 2, "character": 11})

	def test_cancelled_request(self):
		self.client.notify("$/cancelRequest", {"id": 9999})
		self.client.send({"jsonrpc": "2.0", "id": 9999, "method": "lunaut/status", "params": {}})
		response = self.client.wait_response(9999)
		self.assertEqual(response["error"]["code"], -32800)


class RojoNavigationTests(unittest.TestCase):
	def test_string_require_action(self):
		client = LspClient(FIXTURES / "rojo")

		try:
			path = "src/server/Main.server.luau"
			text = (FIXTURES / "rojo" / path).read_text(encoding="utf-8")
			uri, _ = client.open(path)

			line, character = position_of(text, "script.Parent.Helper", 3)
			titles = [action["title"] for action in code_actions(client, uri, line, character)]
			self.assertIn('Use string require "./Helper"', titles)

			line, character = position_of(text, "ReplicatedStorage.Shared.Util", 20)
			titles = [action["title"] for action in code_actions(client, uri, line, character)]
			self.assertIn('Use string require "@game/ReplicatedStorage/Shared/Util"', titles)
		finally:
			client.close()

	def test_studio_plugin_tree(self):
		client = LspClient(FIXTURES / "rojo")

		try:
			path = "src/server/Main.server.luau"
			text = (FIXTURES / "rojo" / path).read_text(encoding="utf-8")
			uri, _ = client.open(path)
			tree = {
				"Name": "game",
				"ClassName": "DataModel",
				"Children": [
					{
						"Name": "ServerScriptService",
						"ClassName": "ServerScriptService",
						"Children": [{"Name": "Server", "ClassName": "Folder", "Children": [{"Name": "Settings", "ClassName": "Folder"}]}],
					}
				],
			}
			client.notify("$/plugin/full", tree)
			client.change(uri, "local parent = script.Parent.\n", version=5)
			labels = {item["label"] for item in client.at("textDocument/completion", uri, 0, len("local parent = script.Parent."))["items"]}
			self.assertTrue({"Settings", "Helper", "Main"} <= labels, labels)

			client.notify("$/plugin/clear", None)
			client.change(uri, "local parent = script.Parent.\n", version=6)
			labels = {item["label"] for item in client.at("textDocument/completion", uri, 0, len("local parent = script.Parent."))["items"]}
			self.assertNotIn("Settings", labels)
			self.assertIn("Helper", labels)
			client.change(uri, text, version=7)
		finally:
			client.close()


class WorkspaceDiagnosticsTests(unittest.TestCase):
	def test_unopened_files_get_diagnostics(self):
		client = LspClient(FIXTURES / "basic", {"fflags": {"sync": False}, "diagnostics": {"workspace": True}})

		try:
			broken = to_key((FIXTURES / "basic" / "broken.luau").resolve().as_uri())
			params = client.wait_for("textDocument/publishDiagnostics", lambda params: to_key(params["uri"]) == broken, timeout=60)
			self.assertTrue(any("does_not_exist" in diagnostic["message"] for diagnostic in params["diagnostics"]))
		finally:
			client.close()


if __name__ == "__main__":
	unittest.main()
