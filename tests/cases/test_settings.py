import json
import pathlib
import shutil
import tempfile
import unittest

from lsp_client import FIXTURES, LspClient, run_cli

FULL_RANGE = {"start": {"line": 0, "character": 0}, "end": {"line": 999, "character": 0}}
LONG_TABLE = "local long = { alpha = 1, bravo = 2, charlie = 3, delta = 4, echo = 5, foxtrot = 6, golf = 7, hotel = 8 }\nreturn long\n"
CYCLIC_METATABLE = """local Complex = {}

local function complex(x, y)
	return setmetatable({ re = x, im = y }, Complex.metatable)
end

function Complex.add(x, y)
	return complex(x.re + y.re, x.im + y.im)
end

function Complex.mul(x, y)
	return complex(x.re * y.re - x.im * y.im, x.re * y.im + x.im * y.re)
end

Complex.metatable = { __add = Complex.add, __mul = Complex.mul }

function level(x, y)
	local c = complex(x, y)
	local l = 0
	local z = c
	repeat
		z = z * z + c
		l = l + 1
	until l > 255
	return l - 1
end
"""


def write(root, path, text):
	file = pathlib.Path(root) / path
	file.parent.mkdir(parents=True, exist_ok=True)
	file.write_text(text, encoding="utf-8")


class SettingsTests(unittest.TestCase):
	def setUp(self):
		self.folder = tempfile.mkdtemp()
		self.addCleanup(shutil.rmtree, self.folder, True)

	def start(self, root, settings):
		client = LspClient(root, {"vscode": {"lunaut.fflags.sync": False, **settings}})
		self.addCleanup(client.close)
		return client

	def hints(self, client, uri):
		return client.request("textDocument/inlayHint", {"textDocument": {"uri": uri}, "range": FULL_RANGE})

	def labels(self, client, uri):
		return [(hint["position"]["line"], hint["label"]) for hint in self.hints(client, uri)]

	def test_type_hint_max_length(self):
		folder = self.folder
		write(folder, "long.luau", LONG_TABLE)
		short = self.start(folder, {"lunaut.inlayHints.typeHintMaxLength": 20})
		uri, _ = short.open("long.luau")
		hint = next(hint for hint in self.hints(short, uri) if hint["position"]["line"] == 0)
		self.assertLessEqual(len(hint["label"]), 2 + 20 + 3, hint)
		self.assertTrue(hint["label"].endswith("..."), hint)
		self.assertIn("alpha", hint["tooltip"])
		self.assertNotIn("textEdits", hint)

		wide = self.start(folder, {"lunaut.inlayHints.typeHintMaxLength": 500})
		uri, _ = wide.open("long.luau")
		hint = next(hint for hint in self.hints(wide, uri) if hint["position"]["line"] == 0)
		self.assertIn("hotel", hint["label"])
		self.assertNotIn("tooltip", hint)

	def test_stringifier_length_fflags(self):
		folder = self.folder
		write(folder, "long.luau", LONG_TABLE)

		for flag in ("LuauTableTypeMaximumStringifierLength", "LuauTypeMaximumStringifierLength"):
			client = self.start(folder, {"lunaut.fflags.override": {flag: "30"}})
			uri, _ = client.open("long.luau")
			hover = client.request("textDocument/hover", {"textDocument": {"uri": uri}, "position": {"line": 1, "character": 8}})
			text = hover["contents"]["value"]
			self.assertIn("*TRUNCATED*", text, flag)
			self.assertNotIn("hotel", text, flag)
			client.close()

		client = self.start(folder, {})
		uri, _ = client.open("long.luau")
		hover = client.request("textDocument/hover", {"textDocument": {"uri": uri}, "position": {"line": 1, "character": 8}})
		self.assertIn("hotel", hover["contents"]["value"])

	def test_no_type_hint_after_an_existing_annotation(self):
		folder = self.folder
		write(folder, "typed.luau", "type item = { name: string }\nlocal value: item = { name = 1 :: any }\nconst other: item = value :\nreturn value\n")
		client = self.start(folder, {})
		uri, _ = client.open("typed.luau")
		self.assertFalse([label for line, label in self.labels(client, uri) if line in (1, 2) and label.startswith(":")])

	def test_cyclic_metatable_does_not_crash_inlay_hints(self):
		folder = self.folder
		write(folder, "complex.luau", CYCLIC_METATABLE)
		client = self.start(folder, {})
		uri, _ = client.open("complex.luau")
		self.assertTrue(self.hints(client, uri))
		self.assertIsNone(client.process.poll())

	def test_empty_table_is_not_an_empty_alias(self):
		folder = self.folder
		write(folder, "types/empty.d.luau", "type EmptyOptions = { }\n")
		write(folder, "empty.luau", "const class = {}\nlocal other = {}\nreturn { class, other }\n")
		client = self.start(folder, {})
		uri, _ = client.open("empty.luau")
		labels = self.labels(client, uri)
		self.assertIn((0, ": {}"), labels)
		self.assertIn((1, ": {}"), labels)

	def test_hide_hints_for_matching_parameter_names(self):
		folder = self.folder
		write(folder, "names.luau", "local function scale(value: number, factor: number)\n\treturn value * factor\nend\nlocal value = 2\nreturn scale(value, 3)\n")

		for hide, expected in ((True, False), (False, True)):
			client = self.start(
				folder, {"lunaut.inlayHints.parameterNames": "all", "lunaut.inlayHints.hideHintsForMatchingParameterNames": hide}
			)
			uri, _ = client.open("names.luau")
			self.assertEqual((4, "value:") in self.labels(client, uri), expected, hide)
			client.close()

	def test_make_insertable(self):
		folder = self.folder
		write(folder, "insert.luau", 'local text = "hello"\nreturn text\n')

		for insertable in (True, False):
			client = self.start(folder, {"lunaut.inlayHints.makeInsertable": insertable})
			uri, _ = client.open("insert.luau")
			hint = next(hint for hint in self.hints(client, uri) if hint["label"] == ": string")
			self.assertEqual("textEdits" in hint, insertable)
			client.close()

	def test_hide_hints_for_error_types(self):
		folder = self.folder
		write(folder, "error.luau", "local broken = missing.value\nreturn broken\n")

		for hide, expected in ((True, False), (False, True)):
			client = self.start(folder, {"lunaut.inlayHints.hideHintsForErrorTypes": hide})
			uri, _ = client.open("error.luau")
			self.assertEqual(any(line == 0 for line, _ in self.labels(client, uri)), expected, hide)
			client.close()

	def test_semantic_tokens_setting(self):
		folder = self.folder
		write(folder, "tokens.luau", "local value = 1\nreturn value\n")

		for enabled in (True, False):
			client = self.start(folder, {"lunaut.semanticTokens.enabled": enabled})
			uri, _ = client.open("tokens.luau")
			result = client.request("textDocument/semanticTokens/full", {"textDocument": {"uri": uri}})
			self.assertEqual(bool(result and result.get("data")), enabled)
			client.close()

	def test_colors_setting(self):
		folder = self.folder
		write(folder, "colors.luau", "local tint = Color3.new(1, 0, 0)\nreturn tint\n")

		for enabled in (True, False):
			client = self.start(folder, {"lunaut.platform": "roblox", "lunaut.roblox.downloadTypes": False, "lunaut.colors.enabled": enabled})
			uri, _ = client.open("colors.luau")
			colors = client.request("textDocument/documentColor", {"textDocument": {"uri": uri}})
			self.assertEqual(bool(colors), enabled)
			client.close()

	def test_code_lens_setting(self):
		folder = self.folder
		write(folder, "lens.luau", "local function helper()\n\treturn 1\nend\nreturn helper()\n")

		for enabled in (True, False):
			client = self.start(folder, {"lunaut.codeLens.references": enabled})
			uri, _ = client.open("lens.luau")
			lenses = client.request("textDocument/codeLens", {"textDocument": {"uri": uri}})
			self.assertEqual(bool(lenses), enabled)
			client.close()

	def test_convert_quotes_setting(self):
		folder = self.folder
		write(folder, "quotes.luau", 'local s = "hello {"\n')

		for enabled in (True, False):
			client = self.start(folder, {"lunaut.format.convertQuotes": enabled})
			uri, _ = client.open("quotes.luau")
			edits = client.request(
				"textDocument/onTypeFormatting",
				{"textDocument": {"uri": uri}, "position": {"line": 0, "character": 17}, "ch": "{", "options": {"tabSize": 4, "insertSpaces": False}},
			)
			self.assertEqual(bool(edits), enabled)
			client.close()

	def test_close_interpolation_setting(self):
		folder = self.folder
		write(folder, "quotes.luau", 'local s = "hello {"\n')

		for enabled, expected in ((True, ["`", "}`"]), (False, ["`", "`"])):
			client = self.start(folder, {"lunaut.format.closeInterpolation": enabled})
			uri, _ = client.open("quotes.luau")
			edits = client.request(
				"textDocument/onTypeFormatting",
				{"textDocument": {"uri": uri}, "position": {"line": 0, "character": 17}, "ch": "{", "options": {"tabSize": 4, "insertSpaces": False}},
			)
			self.assertEqual(sorted(edit["newText"] for edit in edits), sorted(expected), enabled)
			client.close()

	def test_autocomplete_end_setting(self):
		folder = self.folder
		write(folder, "ending.luau", "local function test()\n\t\n")

		for enabled in (True, False):
			client = self.start(folder, {"lunaut.completion.autocompleteEnd": enabled})
			uri, _ = client.open("ending.luau")
			edits = client.request(
				"textDocument/onTypeFormatting",
				{"textDocument": {"uri": uri}, "position": {"line": 1, "character": 1}, "ch": "\n", "options": {"tabSize": 4, "insertSpaces": False}},
			)
			self.assertEqual(bool(edits), enabled)
			client.close()

	def test_format_enabled_setting(self):
		folder = self.folder
		write(folder, "format.luau", "local x=1\nreturn x\n")
		client = self.start(folder, {"lunaut.format.enabled": False})
		uri, _ = client.open("format.luau")
		edits = client.request("textDocument/formatting", {"textDocument": {"uri": uri}, "options": {"tabSize": 4, "insertSpaces": False}})
		self.assertFalse(edits)

	def test_auto_imports_setting(self):
		folder = self.folder
		write(folder, "helpers.luau", "return { greet = function() return 1 end }\n")
		write(folder, "main.luau", "local result = help\nreturn result\n")

		for enabled in (True, False):
			client = self.start(folder, {"lunaut.completion.autoImports": enabled})
			client.open("helpers.luau")
			uri, _ = client.open("main.luau")
			completion = client.request("textDocument/completion", {"textDocument": {"uri": uri}, "position": {"line": 0, "character": 19}})
			items = completion["items"] if isinstance(completion, dict) else completion
			imported = [item for item in items if item["label"] == "helpers" and item.get("additionalTextEdits")]
			self.assertEqual(bool(imported), enabled)
			client.close()

	def test_require_style_settings(self):
		folder = self.folder
		package = "src/static/system/reflection/src/package"
		write(folder, "src/.luaurc", json.dumps({"aliases": {"static": "static", "reflection": "static/system/reflection/src"}}))
		write(folder, package + "/members/BaseMember.luau", "return { base = 1 }\n")
		write(folder, package + "/ReflectedOutput.luau", "return { output = 1 }\n")
		write(folder, package + "/members/User.luau", "local a = BaseMem\nlocal b = ReflectedOut\nreturn { a, b }\n")

		def imports(settings):
			client = self.start(folder, settings)
			client.open(package + "/members/BaseMember.luau")
			client.open(package + "/ReflectedOutput.luau")
			uri, _ = client.open(package + "/members/User.luau")
			found = {}

			for line, character, label in ((0, 17, "BaseMember"), (1, 22, "ReflectedOutput")):
				completion = client.request("textDocument/completion", {"textDocument": {"uri": uri}, "position": {"line": line, "character": character}})
				items = completion["items"] if isinstance(completion, dict) else completion
				item = next(item for item in items if item["label"] == label and item.get("additionalTextEdits"))
				found[label] = item["additionalTextEdits"][0]["newText"]

			client.close()
			return found

		default = imports({})
		self.assertIn('require("./BaseMember")', default["BaseMember"])
		self.assertIn('require("@reflection/package/ReflectedOutput")', default["ReflectedOutput"])

		longest = imports({"lunaut.completion.imports.aliasPath": "longest"})
		self.assertIn('require("@static/system/reflection/src/package/ReflectedOutput")', longest["ReflectedOutput"])

		relative = imports({"lunaut.completion.imports.requireStyle": "alwaysRelative"})
		self.assertIn('require("../ReflectedOutput")', relative["ReflectedOutput"])

		absolute = imports({"lunaut.completion.imports.requireStyle": "alwaysAbsolute"})
		self.assertIn('require("@reflection/package/members/BaseMember")', absolute["BaseMember"])

		compatible = imports({"luau-lsp.completion.imports.requireStyle": "alwaysRelative"})
		self.assertIn('require("../ReflectedOutput")', compatible["ReflectedOutput"])

	def test_lint_setting(self):
		folder = self.folder
		write(folder, "lint.luau", "local unused = 1\nreturn nil\n")

		for enabled in (True, False):
			write(folder, ".vscode/settings.json", json.dumps({"lunaut.diagnostics.lint": enabled}))
			output = run_cli("analyze", "lint.luau", cwd=folder).stdout
			self.assertEqual("LocalUnused" in output, enabled, output)

	def test_enable_by_default_setting(self):
		folder = self.folder
		for enabled in (True, False):
			write(folder, ".vscode/settings.json", json.dumps({"lunaut.fflags.enableByDefault": enabled, "lunaut.fflags.sync": False}))
			client = LspClient(folder, {"vscode": {"lunaut.fflags.sync": False}})
			flags = client.request("lunaut/fflags", {"filter": ""})
			client.close()
			enabled_flags = [flag for flag in flags if flag["source"] == "EnableByDefault" and flag["value"] == "true"]
			self.assertEqual(len(enabled_flags) > 0, enabled)


if __name__ == "__main__":
	unittest.main()
