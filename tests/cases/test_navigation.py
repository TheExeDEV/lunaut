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

	def apply_on_type_end(self, name, text, line, character, spaces=False):
		uri, _ = self.client.open(name, text)
		edits = self.client.request(
			"textDocument/onTypeFormatting",
			{"textDocument": {"uri": uri}, "position": {"line": line, "character": character}, "ch": "\n", "options": {"tabSize": 4, "insertSpaces": spaces}},
		)
		lines = text.split("\n")

		def offset(position):
			return sum(len(item) + 1 for item in lines[: position["line"]]) + position["character"]

		for edit in sorted(edits or [], key=lambda edit: offset(edit["range"]["start"]), reverse=True):
			text = text[: offset(edit["range"]["start"])] + edit["newText"] + text[offset(edit["range"]["end"]) :]

		return text

	def test_autocomplete_end_before_trailing_text(self):
		cases = [
			("foo(function()\n\t)\n", 1, 1, "foo(function()\n\t\nend)\n"),
			("foo(1, function(x)\n\t, 2)\n", 1, 1, "foo(1, function(x)\n\t\nend, 2)\n"),
			("local t = {\n\tf = function()\n\t\t,\n}\n", 2, 2, "local t = {\n\tf = function()\n\t\t\n\tend,\n}\n"),
			("foo(function()\n\tif x then\n\t\t)\n", 2, 2, "foo(function()\n\tif x then\n\t\t\n\tend\nend)\n"),
			("foo(function()\n\t)", 1, 1, "foo(function()\n\t\nend)"),
			("task.spawn(function()\n\tend)\n", 1, 1, "task.spawn(function()\n\tend)\n"),
			("foo(function()\n\t\n)\n", 1, 1, "foo(function()\n\t\nend)\n"),
			("foo(function()\n\t\n, 2)\n", 1, 1, "foo(function()\n\t\nend, 2)\n"),
			("local t = {\n\tf = function()\n\t\t\n}\n", 2, 2, "local t = {\n\tf = function()\n\t\t\n\tend\n}\n"),
		]

		for index, (text, line, character, expected) in enumerate(cases):
			self.assertEqual(self.apply_on_type_end(f"trailing_end_{index}.luau", text, line, character), expected, text)

	def test_autocomplete_end_after_the_last_line_of_an_open_block(self):
		cases = [
			(
				'for index, item in ipairs({ "a", "b" }) do\n\tprint(index, item)\n\t',
				2,
				1,
				'for index, item in ipairs({ "a", "b" }) do\n\tprint(index, item)\n\t\nend',
			),
			(
				"local function f()\n\tlocal x = 1\n\tif x then\n\t\tprint(x)\n\t\t\n",
				4,
				2,
				"local function f()\n\tlocal x = 1\n\tif x then\n\t\tprint(x)\n\t\t\n\tend\n",
			),
			("local function f()\n\tprint(1)\n\t\nend\n", 2, 1, "local function f()\n\tprint(1)\n\t\nend\n"),
			("for i = 1, 3 do\n\tprint(i)\n\t\n\tprint(i)\n", 2, 1, "for i = 1, 3 do\n\tprint(i)\n\t\n\tprint(i)\n"),
		]

		for index, (text, line, character, expected) in enumerate(cases):
			self.assertEqual(self.apply_on_type_end(f"body_end_{index}.luau", text, line, character), expected, text)

	def test_autocomplete_end_takes_the_indent_of_its_block(self):
		cases = [
			("local function f()\n\tfor i = 1, 3 do\n\t\t\nend\n", 2, 2, "local function f()\n\tfor i = 1, 3 do\n\t\t\n\tend\nend\n"),
			(
				"local function f()\n\tif ready then\n\t\twhile true do\n\t\t\t\n\tend\nend\n",
				3,
				3,
				"local function f()\n\tif ready then\n\t\twhile true do\n\t\t\t\n\t\tend\n\tend\nend\n",
			),
			("local t = {\n\tf = function()\n\t\t\n}\n", 2, 2, "local t = {\n\tf = function()\n\t\t\n\tend\n}\n"),
			("local function f()\n\tlocal g = function()\n\t\t\nend\n", 2, 2, "local function f()\n\tlocal g = function()\n\t\t\n\tend\nend\n"),
			("if a then\n\tprint(a)\nelseif b then\n\t\nend\n", 3, 1, "if a then\n\tprint(a)\nelseif b then\n\t\nend\n"),
			("local function f()\n\tfor i = 1, 3 do\n\t\tprint(i)\n\t\t\n", 3, 2, "local function f()\n\tfor i = 1, 3 do\n\t\tprint(i)\n\t\t\n\tend\n"),
		]

		for index, (text, line, character, expected) in enumerate(cases):
			self.assertEqual(self.apply_on_type_end(f"indent_end_{index}.luau", text, line, character), expected, text)

	def test_autocomplete_end_fixes_the_cursor_line_indent(self):
		cases = [
			("for i = 1, 3 do\n\n", 1, 0, False, "for i = 1, 3 do\n\t\nend\n"),
			("local function f()\n\tfor i = 1, 3 do\n\t\nend\n", 2, 1, False, "local function f()\n\tfor i = 1, 3 do\n\t\t\n\tend\nend\n"),
			("if ready then\n\n", 1, 0, True, "if ready then\n    \nend\n"),
			("local function f()\n    while true do\n    \nend\n", 2, 4, True, "local function f()\n    while true do\n        \n    end\nend\n"),
			("local function f()\n\tfor i = 1, 3 do\n\t\t\nend\n", 2, 2, True, "local function f()\n\tfor i = 1, 3 do\n\t\t\n\tend\nend\n"),
		]

		for index, (text, line, character, spaces, expected) in enumerate(cases):
			self.assertEqual(self.apply_on_type_end(f"cursor_indent_{index}.luau", text, line, character, spaces), expected, text)

	def test_convert_quotes_closes_the_brace(self):
		cases = [
			('local s = "hello {"\n', 17, ["`", "}`"], "local s = `hello {}`\n"),
			('local s = "hello {\n', 17, ["`", "}"], "local s = `hello {}\n"),
			('local s = "hello {}"\n', 17, ["`", "`"], "local s = `hello {}`\n"),
			('local s = "a {b"\n', 13, ["`", "}", "`"], "local s = `a {}b`\n"),
		]

		for index, (text, character, new_texts, expected) in enumerate(cases):
			uri, _ = self.client.open(f"brace_{index}.luau", text)
			edits = self.client.request(
				"textDocument/onTypeFormatting",
				{"textDocument": {"uri": uri}, "position": {"line": 0, "character": character}, "ch": "{", "options": {"tabSize": 4, "insertSpaces": False}},
			)
			self.assertEqual(sorted(edit["newText"] for edit in edits), sorted(new_texts), text)
			result = text

			for edit in sorted(edits, key=lambda edit: edit["range"]["start"]["character"], reverse=True):
				result = result[: edit["range"]["start"]["character"]] + edit["newText"] + result[edit["range"]["end"]["character"] :]

			self.assertEqual(result, expected, text)

	def test_autocomplete_end_on_type(self):
		self.assertEqual(self.client.capabilities["documentOnTypeFormattingProvider"]["moreTriggerCharacter"], ["\n"])
		self.assertNotIn("\n", self.client.capabilities["completionProvider"]["triggerCharacters"])

		cases = [
			("local function test()\n\t\n", {"line": 1, "character": 1}, ["end\n"]),
			("if ready then\n\t\n", {"line": 1, "character": 1}, ["end\n"]),
			("for i = 1, 3\n\t\n", {"line": 1, "character": 1}, [" do", "end\n"]),
			("repeat\n\t\n", {"line": 1, "character": 1}, ["until \n"]),
			("local function done()\n\t\nend\n", {"line": 1, "character": 1}, None),
		]

		for index, (text, position, expected) in enumerate(cases):
			uri, _ = self.client.open(f"typed_end_{index}.luau", text)
			edits = self.client.request(
				"textDocument/onTypeFormatting",
				{"textDocument": {"uri": uri}, "position": position, "ch": "\n", "options": {"tabSize": 4, "insertSpaces": False}},
			)
			self.assertEqual(None if edits is None else [edit["newText"] for edit in edits], expected, text)

	def test_vscode_extension_gets_end_on_type(self):
		client = LspClient(FIXTURES / "basic", {"vscode": {"lunaut.fflags.sync": False}, "client": "lunaut-vscode"})

		try:
			self.assertEqual(client.capabilities["documentOnTypeFormattingProvider"]["moreTriggerCharacter"], ["\n"])
			uri, _ = client.open("ending_vscode.luau", "local function test()\n\t\n")
			edits = client.request(
				"textDocument/onTypeFormatting",
				{"textDocument": {"uri": uri}, "position": {"line": 1, "character": 1}, "ch": "\n", "options": {"tabSize": 4, "insertSpaces": False}},
			)
			self.assertEqual([edit["newText"] for edit in edits], ["end\n"])
		finally:
			client.close()

	def test_convert_quotes(self):
		uri, _ = self.client.open("quotes.luau", 'local s = "hello {"\n')
		edits = self.client.request(
			"textDocument/onTypeFormatting",
			{"textDocument": {"uri": uri}, "position": {"line": 0, "character": 17}, "ch": "{", "options": {"tabSize": 4, "insertSpaces": False}},
		)
		self.assertEqual([(edit["range"]["start"]["character"], edit["newText"]) for edit in edits], [(10, "`"), (18, "}`")])

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
