import unittest

from lsp_client import FIXTURES, LspClient, position_of


def full_range():
	return {"start": {"line": 0, "character": 0}, "end": {"line": 999, "character": 0}}


def decode_tokens(data):
	tokens = []
	line = 0
	column = 0

	for index in range(0, len(data), 5):
		delta_line, delta_column, length, kind, modifiers = data[index : index + 5]
		line += delta_line
		column = column + delta_column if delta_line == 0 else delta_column
		tokens.append((line, column, length, kind, modifiers))

	return tokens


class FeatureTests(unittest.TestCase):
	@classmethod
	def setUpClass(cls):
		cls.client = LspClient(FIXTURES / "basic")
		cls.hints_uri, _ = cls.client.open("hints.luau")
		cls.documented = (FIXTURES / "basic" / "documented.luau").read_text(encoding="utf-8")
		cls.documented_uri, _ = cls.client.open("documented.luau")

	@classmethod
	def tearDownClass(cls):
		cls.client.close()

	def hints(self, client, uri):
		hints = client.request("textDocument/inlayHint", {"textDocument": {"uri": uri}, "range": full_range()})
		return [(hint["position"]["line"], hint["label"]) for hint in hints]

	def test_inlay_hints_default(self):
		hints = self.hints(self.client, self.hints_uri)
		self.assertIn((1, ": number"), hints)
		self.assertIn((5, ": number"), hints)
		self.assertIn((5, "value:"), hints)
		self.assertIn((5, "factor:"), hints)
		self.assertIn((6, ": string"), hints)
		self.assertIn((7, ": number"), hints)

	def test_inlay_hints_insertable(self):
		hints = self.client.request("textDocument/inlayHint", {"textDocument": {"uri": self.hints_uri}, "range": full_range()})
		typed = next(hint for hint in hints if hint["label"] == ": string")
		self.assertEqual(typed["textEdits"][0]["newText"], ": string")

	def test_inlay_hints_can_be_disabled(self):
		client = LspClient(
			FIXTURES / "basic",
			{
				"fflags": {"sync": False},
				"inlay_hints": {"variable_types": False, "parameter_names": "none", "function_return_types": False},
			},
		)

		try:
			uri, _ = client.open("hints.luau")
			self.assertEqual(self.hints(client, uri), [])
		finally:
			client.close()

	def test_inlay_hints_parameter_types(self):
		client = LspClient(FIXTURES / "basic", {"fflags": {"sync": False}, "inlay_hints": {"parameter_types": True}})

		try:
			uri, _ = client.open("documented.luau", "local function twice(value)\n\treturn value * 2\nend\nprint(twice(1))\n")
			hints = client.request("textDocument/inlayHint", {"textDocument": {"uri": uri}, "range": full_range()})
			after_value = [hint for hint in hints if hint["position"] == {"line": 0, "character": len("local function twice(value")}]
			self.assertEqual(len(after_value), 1, hints)
			self.assertTrue(after_value[0]["label"].startswith(": "))
		finally:
			client.close()

	def test_hover_moonwave_documentation(self):
		line, character = position_of(self.documented, "double(2)", 1)
		value = self.client.at("textDocument/hover", self.documented_uri, line, character)["contents"]["value"]
		self.assertIn("function double(value: number): number", value)
		self.assertIn("\n----------\nDoubles a number.", value)
		self.assertIn("**Parameters**\n- `value` number to double", value)
		self.assertIn("**Returns**\n- the doubled number", value)

	def test_hover_line_comment_documentation(self):
		line, character = position_of(self.documented, 'greet("', 1)
		value = self.client.at("textDocument/hover", self.documented_uri, line, character)["contents"]["value"]
		self.assertTrue(value.endswith("----------\nGreets someone by name."), value)

	def test_hover_string_length(self):
		line, character = position_of(self.documented, '"lunaut"', 2)
		value = self.client.at("textDocument/hover", self.documented_uri, line, character)["contents"]["value"]
		self.assertIn("string (6 bytes)", value)

	def test_completion_function_details(self):
		uri, _ = self.client.open("calls.luau", "local util = require(\"./util\")\nutil.\n")
		items = self.client.at("textDocument/completion", uri, 1, 5)["items"]
		add = next(item for item in items if item["label"] == "add")
		self.assertEqual(add["labelDetails"]["detail"], "(a, b)")
		self.assertEqual(add["insertText"], "add(${1:a}, ${2:b})$0")
		self.assertEqual(add["insertTextFormat"], 2)

	def test_auto_import_modules(self):
		uri, _ = self.client.open("imports.luau", "--!strict\nlocal x = mat\n")
		items = self.client.at("textDocument/completion", uri, 1, 13)["items"]
		math2 = next(item for item in items if item["label"] == "math2")
		edit = math2["additionalTextEdits"][0]
		self.assertEqual(edit["newText"], 'local math2 = require("./lib/math2")\n')
		self.assertEqual(edit["range"]["start"]["line"], 1)

	def test_semantic_tokens(self):
		data = self.client.request("textDocument/semanticTokens/full", {"textDocument": {"uri": self.hints_uri}})["data"]
		tokens = decode_tokens(data)
		self.assertIn((1, 15, 5, 12, 1), tokens)
		self.assertIn((1, 21, 5, 7, 1), tokens)
		self.assertIn((5, 6, 6, 8, 1), tokens)

	def test_code_actions_for_lints(self):
		uri, diagnostics = self.client.open("main.luau")
		unused = [diagnostic for diagnostic in diagnostics if diagnostic["code"] == "FunctionUnused"]
		actions = self.client.request(
			"textDocument/codeAction",
			{"textDocument": {"uri": uri}, "range": unused[0]["range"], "context": {"diagnostics": unused}},
		)
		titles = [action["title"] for action in actions]
		self.assertIn("Rename to '_unusedHelper' to silence FunctionUnused", titles)
		self.assertIn("Disable FunctionUnused in this file", titles)

	def test_code_action_adds_require(self):
		uri, diagnostics = self.client.open("unknown.luau", "--!strict\nprint(math2.square(2))\n")
		actions = self.client.request(
			"textDocument/codeAction",
			{"textDocument": {"uri": uri}, "range": diagnostics[0]["range"], "context": {"diagnostics": diagnostics}},
		)
		edit = actions[0]["edit"]["changes"][uri][0]
		self.assertEqual(edit["newText"], 'local math2 = require("./lib/math2")\n')

	def test_document_colors(self):
		uri, _ = self.client.open("colors.luau", "local red = Color3.fromRGB(255, 0, 0)\nlocal sky = Color3.fromHex(\"#3399FF\")\n")
		colors = self.client.request("textDocument/documentColor", {"textDocument": {"uri": uri}})
		self.assertEqual(colors[0]["color"], {"red": 1.0, "green": 0.0, "blue": 0.0, "alpha": 1.0})
		self.assertAlmostEqual(colors[1]["color"]["blue"], 1.0)

		presentations = self.client.request(
			"textDocument/colorPresentation",
			{"textDocument": {"uri": uri}, "color": {"red": 0, "green": 1, "blue": 0, "alpha": 1}, "range": colors[0]["range"]},
		)
		labels = [presentation["label"] for presentation in presentations]
		self.assertEqual(labels[0], "Color3.fromRGB(0, 255, 0)")
		self.assertIn('Color3.fromHex("#00FF00")', labels)

	def test_folding_ranges(self):
		ranges = self.client.request("textDocument/foldingRange", {"textDocument": {"uri": self.hints_uri}})
		spans = {(fold["startLine"], fold["endLine"]) for fold in ranges}
		self.assertIn((1, 2), spans)
		self.assertIn((7, 8), spans)
		self.assertNotIn((0, 10), spans)

	def test_folding_regions(self):
		text = "--#region setup\nlocal a = 1\nlocal b = 2\n--#endregion\nprint(a, b)\n"
		uri, _ = self.client.open("regions.luau", text)
		ranges = self.client.request("textDocument/foldingRange", {"textDocument": {"uri": uri}})
		self.assertIn({"startLine": 0, "endLine": 3, "kind": "region"}, ranges)


class LuauLspSettingsTests(unittest.TestCase):
	@classmethod
	def setUpClass(cls):
		cls.client = LspClient(FIXTURES / "luau-lsp-settings")
		cls.uri, cls.diagnostics = cls.client.open("main.luau")

	@classmethod
	def tearDownClass(cls):
		cls.client.close()

	def test_definition_files_and_disabled_globals(self):
		messages = [diagnostic["message"] for diagnostic in self.diagnostics]
		self.assertTrue(any("clone" in message for message in messages), messages)
		self.assertFalse(any("area" in message for message in messages), messages)

		definitions = self.client.request("lunaut/definitions", {})[0]["definitions"]
		self.assertIn("@shapes", [definition["name"] for definition in definitions])

	def test_settings_mapped_with_lunaut_priority(self):
		settings = self.client.request("lunaut/status", {})["workspaces"][0]["settings"]
		self.assertEqual(settings["platform"], "standard")
		self.assertFalse(settings["inlay_hints"]["variable_types"])
		self.assertEqual(settings["inlay_hints"]["parameter_names"], "none")
		self.assertEqual(settings["disabled_globals"], ["table.clone"])

	def test_vscode_flat_settings(self):
		client = LspClient(
			FIXTURES / "basic",
			{"vscode": {"luau-lsp.fflags.sync": False, "luau-lsp.inlayHints.functionReturnTypes": False, "lunaut.inlayHints.variableTypes": False}},
		)

		try:
			settings = client.request("lunaut/status", {})["workspaces"][0]["settings"]
			self.assertFalse(settings["inlay_hints"]["function_return_types"])
			self.assertFalse(settings["inlay_hints"]["variable_types"])
		finally:
			client.close()


class RobloxDocumentationTests(unittest.TestCase):
	def test_api_documentation_in_hover_and_completion(self):
		client = LspClient(FIXTURES / "rojo")

		try:
			path = "src/server/Magic.server.luau"
			text = (FIXTURES / "rojo" / path).read_text(encoding="utf-8")
			uri, _ = client.open(path)
			line, character = position_of(text, "FindFirstChildWhichIsA", 3)
			value = client.at("textDocument/hover", uri, line, character)["contents"]["value"]
			self.assertIn("Returns the first child that is of the given class.", value)
			self.assertIn("[Learn More](https://create.roblox.com/docs/reference/engine/classes/Instance#FindFirstChildWhichIsA)", value)
			self.assertIn('```luau\nlocal model = workspace:FindFirstChildWhichIsA("Model")\n```', value)
			self.assertNotIn("/param/", value)

			call = 'game:FindFirstChildWhichIsA("'
			line, character = position_of(text, call, len(call))
			help = client.at("textDocument/signatureHelp", uri, line, character)
			signature = help["signatures"][0]
			self.assertIn("function game:FindFirstChildWhichIsA(className: string", signature["label"])
			self.assertIn("Returns the first child that is of the given class.", signature["documentation"]["value"])
			self.assertEqual(signature["parameters"][0]["documentation"]["value"], "The class to look for.")
		finally:
			client.close()

	def test_service_auto_import(self):
		client = LspClient(FIXTURES / "rojo")

		try:
			path = "src/server/Main.server.luau"
			uri, _ = client.open(path, "--!strict\nlocal r = Repl\n")
			items = client.at("textDocument/completion", uri, 1, 14)["items"]
			service = next(item for item in items if item["label"] == "ReplicatedStorage")
			self.assertEqual(service["additionalTextEdits"][0]["newText"], 'local ReplicatedStorage = game:GetService("ReplicatedStorage")\n')

			util = next(item for item in items if item["label"] == "Util")
			self.assertEqual(
				util["additionalTextEdits"][0]["newText"], 'local Util = require(game:GetService("ReplicatedStorage").Shared.Util)\n'
			)

			helper = next(item for item in items if item["label"] == "Helper")
			self.assertEqual(helper["additionalTextEdits"][0]["newText"], "local Helper = require(script.Parent.Helper)\n")
		finally:
			client.close()


if __name__ == "__main__":
	unittest.main()
