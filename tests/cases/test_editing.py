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

	def test_odd_member_names_complete_with_brackets(self):
		write(self.folder, "odd.luau", 'local rig = { ["Accessory (D4)"] = 1, ["say \\"hi\\""] = 2, plain = 3, ["end"] = 4 }\nlocal a = rig.\nlocal b = rig.Acc\nreturn { a, b }\n')
		client = self.start()
		uri, _ = client.open("odd.luau")

		def items(line, character):
			result = client.request("textDocument/completion", {"textDocument": {"uri": uri}, "position": {"line": line, "character": character}})
			return {item["label"]: item for item in (result["items"] if isinstance(result, dict) else result)}

		after_dot = items(1, 14)
		odd = after_dot["Accessory (D4)"]
		self.assertEqual(odd["textEdit"]["newText"], '["Accessory (D4)"]')
		self.assertEqual(odd["textEdit"]["range"]["start"], {"line": 1, "character": 13})
		self.assertEqual(odd["textEdit"]["range"]["end"], {"line": 1, "character": 14})
		self.assertEqual(after_dot['say "hi"']["textEdit"]["newText"], '["say \\"hi\\""]')
		self.assertEqual(after_dot["end"]["textEdit"]["newText"], '["end"]')
		self.assertNotIn("textEdit", after_dot["plain"])

		typed = items(2, 17)["Accessory (D4)"]
		self.assertEqual(typed["textEdit"]["range"]["start"], {"line": 2, "character": 13})
		self.assertEqual(typed["textEdit"]["range"]["end"], {"line": 2, "character": 17})
		self.assertEqual(typed["filterText"], ".Accessory (D4)")

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

	def labels(self, client, uri, line, character):
		result = client.request("textDocument/completion", {"textDocument": {"uri": uri}, "position": {"line": line, "character": character}})
		return {item["label"] for item in (result["items"] if isinstance(result, dict) else result)}

	def test_show_as_module_types_complete_and_need_the_module_name(self):
		write(self.folder, "types/base.d.luau", "export type Vehicle = { speed: number }\nexport type Wheel = { size: number }\ndeclare function make_vehicle(): Vehicle\n")
		write(self.folder, "types/later.d.luau", "declare function make_wheel(): base.Wheel\n")
		write(self.folder, "main.luau", "--!strict\nlocal car: base.Vehicle = make_vehicle()\nlocal plain: Vehicle = make_vehicle()\nlocal wheel: base.Wheel = make_wheel()\nlocal pick: b = nil\nreturn { car, plain, wheel, pick }\n")
		definitions = [{"path": "types/base.d.luau", "name": "@base", "show_as_module": True, "priority": 10}, "types/later.d.luau"]
		client = self.start({"lunaut.definitions": definitions, "lunaut.discoverDefinitions": False})
		uri, diagnostics = client.open("main.luau")

		messages = [diagnostic["message"] for diagnostic in diagnostics]
		self.assertIn("Unknown type 'Vehicle'", messages)
		self.assertIn("Unknown type 'b'", messages)
		self.assertEqual(len(messages), 2, messages)
		self.assertTrue(all(item["success"] for item in client.request("lunaut/definitions", {})[0]["definitions"]))

		in_type = self.labels(client, uri, 4, 13)
		self.assertIn("base", in_type)
		self.assertNotIn("Vehicle", in_type)
		self.assertEqual(self.labels(client, uri, 1, 17) & {"Vehicle", "Wheel", "base"}, {"Vehicle", "Wheel"})

		hover = client.request("textDocument/hover", {"textDocument": {"uri": uri}, "position": {"line": 1, "character": 17}})
		self.assertIn("type base.Vehicle", hover["contents"]["value"])

	def test_show_as_module_directive_in_the_file(self):
		write(self.folder, "types/base.d.luau", "--!lunaut_show_as_module true\nexport type Vehicle = { speed: number }\n")
		write(self.folder, "types/flat.d.luau", "--!lunaut_show_as_module true\nexport type Flat = { size: number }\n")
		write(self.folder, "main.luau", "--!strict\nlocal a: base.Vehicle = { speed = 1 }\nlocal b: Vehicle = { speed = 1 }\nlocal c: Flat = { size = 1 }\nreturn { a, b, c }\n")
		definitions = ["types/base.d.luau", {"path": "types/flat.d.luau", "show_as_module": False}]
		client = self.start({"lunaut.definitions": definitions, "lunaut.discoverDefinitions": False})
		_, diagnostics = client.open("main.luau")

		self.assertEqual([diagnostic["message"] for diagnostic in diagnostics], ["Unknown type 'Vehicle'"])
		listed = {item["name"]: item["show_as_module"] for item in client.request("lunaut/definitions", {})[0]["definitions"]}
		self.assertEqual(listed, {"@base": True, "@flat": False})

	def test_saving_definitions_can_skip_the_reload(self):
		first = write(self.folder, "types/first.d.luau", "declare first_value: number\n")
		write(self.folder, "main.luau", "--!strict\nlocal a: number = first_value\nreturn a\n")
		settings = {"lunaut.definitions": ["types/first.d.luau"], "lunaut.discoverDefinitions": False, "lunaut.restartOnDefinitionChanges": False}
		client = self.start(settings)
		uri, diagnostics = client.open("main.luau")
		self.assertEqual(diagnostics, [])

		first.write_text("declare other_value: number\n", encoding="utf-8")
		client.notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": to_uri(first), "type": 2}]})
		client.request("lunaut/status", {})
		hover = client.request("textDocument/hover", {"textDocument": {"uri": uri}, "position": {"line": 1, "character": 20}})
		self.assertIn("number", hover["contents"]["value"])

		client.request("workspace/executeCommand", {"command": "lunaut.server.reloadWorkspace", "arguments": []})
		broken = client.wait_for("textDocument/publishDiagnostics", lambda params: params["uri"].lower() == uri.lower() and any("first_value" in item["message"] for item in params["diagnostics"]), timeout=60)
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
