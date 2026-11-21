import pathlib
import shutil
import tempfile
import unittest

from lsp_client import LspClient, run_cli, to_uri


def write(root, path, text):
	file = pathlib.Path(root) / path
	file.parent.mkdir(parents=True, exist_ok=True)
	file.write_text(text, encoding="utf-8")
	return file


class EditingTests(unittest.TestCase):
	def setUp(self):
		self.folder = tempfile.mkdtemp()
		self.addCleanup(shutil.rmtree, self.folder, True)

	def start(self, settings=None):
		client = LspClient(self.folder, {"vscode": {"lunaut.fflags.sync": False, **(settings or {})}})
		self.addCleanup(client.close)
		return client

	def rename(self, client, file, line, character, name):
		uri, _ = client.open(file)
		result = client.request("textDocument/rename", {"textDocument": {"uri": uri}, "position": {"line": line, "character": character}, "newName": name})
		return {pathlib.Path(key).name: len(value) for key, value in result["changes"].items()}

	def test_unknown_require_is_reported_in_every_mode(self):
		write(self.folder, "real.luau", "return 1\n")
		write(self.folder, "loose.luau", 'local a = require("./missing")\nlocal b = require("./real")\nreturn { a, b }\n')
		write(self.folder, "strict.luau", '--!strict\nlocal a = require("./missing")\nreturn a\n')

		for file in ("loose.luau", "strict.luau"):
			output = run_cli("analyze", file, cwd=self.folder).stdout
			self.assertEqual(output.count("Unknown require"), 1, output)

		output = run_cli("analyze", "--flag:LuauSolverV2=true", "loose.luau", cwd=self.folder).stdout
		self.assertEqual(output.count("Unknown require"), 1, output)

	def test_rename_type_alias_across_modules(self):
		write(self.folder, "types.luau", "export type Point = { x: number }\nlocal function make(): Point\n\treturn { x = 1 }\nend\nreturn { make = make }\n")
		write(self.folder, "user.luau", 'local types = require("./types")\nlocal p: types.Point = { x = 2 }\nreturn p\n')
		client = self.start()
		client.open("user.luau")

		self.assertEqual(self.rename(client, "types.luau", 0, 13, "Spot"), {"types.luau": 2, "user.luau": 1})
		self.assertEqual(self.rename(client, "user.luau", 1, 16, "Spot"), {"types.luau": 2, "user.luau": 1})

	def test_rename_global_across_files(self):
		write(self.folder, "globals.luau", "counter = 0\nfunction bump()\n\tcounter += 1\nend\nreturn counter\n")
		write(self.folder, "other.luau", "bump()\nprint(counter)\nreturn nil\n")
		client = self.start()

		self.assertEqual(self.rename(client, "globals.luau", 0, 2, "count"), {"globals.luau": 3, "other.luau": 1})
		self.assertEqual(self.rename(client, "other.luau", 0, 1, "increment"), {"globals.luau": 1, "other.luau": 1})

		with self.assertRaises(Exception):
			self.rename(client, "other.luau", 1, 2, "show")

	def apply_attribute_fix(self, client, name, text):
		write(self.folder, name, text)
		uri, diagnostics = client.open(name)
		misplaced = [diagnostic for diagnostic in diagnostics if "after attribute" in diagnostic["message"]]
		self.assertEqual(len(misplaced), 1, diagnostics)
		actions = client.request("textDocument/codeAction", {"textDocument": {"uri": uri}, "range": misplaced[0]["range"], "context": {"diagnostics": misplaced}})
		fix = next(action for action in actions if action["title"] == "Move the attribute to the function")
		edits = next(iter(fix["edit"]["changes"].values()))
		lines = text.split("\n")

		for edit in sorted(edits, key=lambda edit: (edit["range"]["start"]["line"], edit["range"]["start"]["character"]), reverse=True):
			start, end = edit["range"]["start"], edit["range"]["end"]

			if start["line"] == end["line"]:
				line = lines[start["line"]]
				lines[start["line"]] = line[: start["character"]] + edit["newText"] + line[end["character"] :]
			else:
				del lines[start["line"] : end["line"]]

		return "\n".join(lines)

	def test_move_attribute_quick_fix(self):
		client = self.start()
		above = self.apply_attribute_fix(client, "above.luau", "local datamodel = {\n\t@native\n\thas_nodes = function(of)\n\t\treturn of\n\tend,\n}\nreturn datamodel\n")
		self.assertEqual(above, "local datamodel = {\n\thas_nodes = @native function(of)\n\t\treturn of\n\tend,\n}\nreturn datamodel\n")

		inline = self.apply_attribute_fix(client, "inline.luau", 'local datamodel = {\n\t@[native, deprecated { use = "x", reason = "y" }] add = function() end,\n}\nreturn datamodel\n')
		self.assertEqual(inline, 'local datamodel = {\n\tadd = @[native, deprecated { use = "x", reason = "y" }] function() end,\n}\nreturn datamodel\n')

		for name, text in (("above_fixed.luau", above), ("inline_fixed.luau", inline)):
			write(self.folder, name, text)
			self.assertNotIn("SyntaxError", run_cli("analyze", name, cwd=self.folder).stdout, text)

	def test_attribute_completion(self):
		write(self.folder, "complete.luau", '@\n@[native, dep\nlocal s = "@na"\n')
		client = self.start()
		uri, _ = client.open("complete.luau")

		def labels(line, character):
			result = client.request("textDocument/completion", {"textDocument": {"uri": uri}, "position": {"line": line, "character": character}})
			items = result["items"] if isinstance(result, dict) else result
			return [item["label"] for item in items]

		self.assertEqual(labels(0, 1), ["native", "deprecated"])
		self.assertEqual(labels(1, 14), ["native", "deprecated", "deprecated { use, reason }"])
		self.assertNotIn("native", labels(2, 13))

	def test_definitions_reload_without_restart(self):
		write(self.folder, "types/first.d.luau", "declare first_value: number\n")
		write(self.folder, "main.luau", "--!strict\nlocal a: number = first_value\nlocal b: number = second_value\nreturn a + b\n")
		client = self.start({"lunaut.definitions": ["types/first.d.luau"], "lunaut.discoverDefinitions": False})
		uri, diagnostics = client.open("main.luau")
		self.assertTrue(any("second_value" in diagnostic["message"] for diagnostic in diagnostics), diagnostics)

		second = write(self.folder, "types/second.d.luau", "declare second_value: number\n")
		client.notify(
			"workspace/didChangeConfiguration",
			{"settings": {"vscode": {"lunaut.fflags.sync": False, "lunaut.discoverDefinitions": False, "lunaut.definitions": ["types/first.d.luau", "types/second.d.luau"]}}},
		)
		fresh = client.wait_for("textDocument/publishDiagnostics", lambda params: params["uri"].lower() == uri.lower() and not any("second_value" in item["message"] for item in params["diagnostics"]), timeout=60)
		self.assertFalse([item for item in fresh["diagnostics"] if "Unknown global" in item["message"]], fresh)

		second.write_text("declare other_value: number\n", encoding="utf-8")
		client.notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": to_uri(second), "type": 2}]})
		broken = client.wait_for("textDocument/publishDiagnostics", lambda params: params["uri"].lower() == uri.lower() and any("second_value" in item["message"] for item in params["diagnostics"]), timeout=60)
		self.assertTrue(broken["diagnostics"])

	def test_fflags_reload_without_restart(self):
		client = self.start({"lunaut.fflags.override": {"LuauTypeMaximumStringifierLength": "123"}})
		flags = {flag["name"]: flag["value"] for flag in client.request("lunaut/fflags", {"filter": "LuauTypeMaximumStringifierLength"})}
		self.assertEqual(flags["LuauTypeMaximumStringifierLength"], "123")

		client.notify("workspace/didChangeConfiguration", {"settings": {"vscode": {"lunaut.fflags.sync": False, "lunaut.fflags.override": {"LuauTypeMaximumStringifierLength": "321"}}}})
		client.request("lunaut/status", {})
		flags = {flag["name"]: flag["value"] for flag in client.request("lunaut/fflags", {"filter": "LuauTypeMaximumStringifierLength"})}
		self.assertEqual(flags["LuauTypeMaximumStringifierLength"], "321")


if __name__ == "__main__":
	unittest.main()
