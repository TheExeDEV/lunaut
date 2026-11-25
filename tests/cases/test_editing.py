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

	def test_show_as_module_message_in_completion(self):
		write(self.folder, "db.d.luau", "export type Row = { id: number }\n")
		write(self.folder, "plain.d.luau", "export type Plain = { id: number }\n")
		write(self.folder, "main.luau", "--!strict\nlocal row: f = nil\nreturn row\n")
		definitions = [
			{"path": "db.d.luau", "name": "@fc_database", "show_as_module": True, "message": "Rows of the funny-core database"},
			{"path": "plain.d.luau", "name": "@plain", "show_as_module": True},
		]
		client = self.start({"lunaut.definitions": definitions, "lunaut.discoverDefinitions": False})
		uri, _ = client.open("main.luau")
		result = client.request("textDocument/completion", {"textDocument": {"uri": uri}, "position": {"line": 1, "character": 12}})
		items = {item["label"]: item for item in (result["items"] if isinstance(result, dict) else result)}

		self.assertEqual(items["fc_database"]["detail"], "Rows of the funny-core database")
		self.assertIn("Rows of the funny-core database", items["fc_database"]["documentation"]["value"])
		self.assertEqual(items["plain"]["detail"], "definitions @plain")
		listed = {item["name"]: item["description"] for item in client.request("lunaut/definitions", {})[0]["definitions"]}
		self.assertEqual(listed["@fc_database"], "Rows of the funny-core database")

	def test_definition_properties_from_directives(self):
		lines = [
			"--!priority 120",
			'--!show_description_message "Rows of the \\"core\\" database"',
			'--!show_module_name_message "fc_db"',
			"--!show_as_module true",
			"export type Row = { id: number }",
		]
		write(self.folder, "types/database.d.luau", "\n".join(lines) + "\n")
		write(self.folder, "main.luau", "--!strict\nlocal row: fc_db.Row = { id = 1 }\nlocal pick: f = nil\nreturn { row, pick }\n")
		client = self.start({"lunaut.definitions": ["types/database.d.luau"], "lunaut.discoverDefinitions": False})
		uri, diagnostics = client.open("main.luau")

		self.assertEqual([diagnostic["message"] for diagnostic in diagnostics], ["Unknown type 'f'"])
		listed = client.request("lunaut/definitions", {})[0]["definitions"][0]
		self.assertEqual((listed["name"], listed["priority"], listed["show_as_module"], listed["description"]), ("@fc_db", 120, True, 'Rows of the "core" database'))
		result = client.request("textDocument/completion", {"textDocument": {"uri": uri}, "position": {"line": 2, "character": 13}})
		items = {item["label"]: item for item in (result["items"] if isinstance(result, dict) else result)}
		self.assertEqual(items["fc_db"]["detail"], 'Rows of the "core" database')

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

	def write_account(self):
		lines = [
			"--!strict",
			"export type Account = { balance: number, owner: string }",
			"local Account = {}",
			"function Account:deposit(amount: number)",
			"\tself.balance += amount",
			"\tlocal wrong: string = self.balance",
			"\tlocal function inner()",
			"\t\treturn self.owner",
			"\tend",
			"\treturn inner()",
			"end",
			"function Account:missing()",
			"\treturn self.nothing",
			"end",
			"local Stack = {}",
			"type Stack<T> = { items: { T } }",
			"function Stack:peek()",
			"\treturn self.anything",
			"end",
			"local Nested = { inner = {} }",
			"type inner = { size: number }",
			"function Nested.inner:grow()",
			"\treturn self.anything",
			"end",
			"return { Account, Stack, Nested }",
		]
		write(self.folder, "account.luau", "\n".join(lines) + "\n")
		write(self.folder, "on.json", '{"lunaut.implicitSelf": true}')

	def test_implicit_self_is_off_by_default(self):
		self.write_account()

		for solver in ("false", "true"):
			output = run_cli("analyze", f"--flag:LuauSolverV2={solver}", "account.luau", cwd=self.folder).stdout
			self.assertNotIn("Key 'nothing' not found", output)

	def test_implicit_self_types_self_from_the_alias(self):
		self.write_account()

		for solver in ("false", "true"):
			output = run_cli("analyze", f"--flag:LuauSolverV2={solver}", "--settings=on.json", "account.luau", cwd=self.folder).stdout
			self.assertIn("account.luau:6:", output)
			self.assertIn("Expected this to be 'string', but got 'number'", output)
			self.assertIn("account.luau:13:9: error [TypeError] Key 'nothing' not found", output)
			self.assertNotIn("owner", output)
			self.assertNotIn("anything", output)

	def test_signature_help_skips_the_receiver_of_a_colon_call(self):
		write(self.folder, "call.luau", "--!strict\nlocal account: { deposit: (any, number) -> () } = nil :: any\naccount:deposit(5)\nreturn account\n")
		client = self.start()
		uri, _ = client.open("call.luau")
		signature = client.request("textDocument/signatureHelp", {"textDocument": {"uri": uri}, "position": {"line": 2, "character": 16}})
		self.assertEqual([parameter["label"] for parameter in signature["signatures"][0]["parameters"]], ["number"])
		self.assertIn("(number)", signature["signatures"][0]["label"])

	def test_show_as_module_types_go_to_their_file(self):
		lines = [
			"export type base_member<T, U> = {",
			"\t--[[",
			"\t\tDescribes the purpose of this member.",
			"\t]]",
			"\tread kind: T,",
			"\tread value: U,",
			"}",
			"",
			"--[==[",
			"ReflectedArgument is the only functional member of the runtime type.",
			"]==]",
			"export type func_member = base_member<\"func\", number>",
			"",
			"--[==[",
			"The members of a reflected class.",
			"]==]",
			"export type library = {",
			"\tread more: () -> func_member,",
			"}",
		]
		write(self.folder, "reflection.d.luau", "\n".join(lines) + "\n")
		write(self.folder, "main.luau", "--!strict\nlocal x: fc_reflection.func_member = nil :: any\nlocal lib: fc_reflection.library = nil :: any\nreturn { x, lib }\n")
		definitions = [{"path": "reflection.d.luau", "name": "@fc_reflection", "show_as_module": True}]
		client = self.start({"lunaut.definitions": definitions, "lunaut.discoverDefinitions": False})
		uri, _ = client.open("main.luau")

		def at(method, line, character):
			return client.request(method, {"textDocument": {"uri": uri}, "position": {"line": line, "character": character}})

		for line, character, target in ((1, 25, 11), (2, 27, 16)):
			location = at("textDocument/definition", line, character)
			self.assertTrue(location["uri"].endswith("reflection.d.luau"), location)
			self.assertEqual(location["range"]["start"]["line"], target)

		hover = at("textDocument/hover", 1, 25)["contents"]["value"]
		self.assertIn("ReflectedArgument is the only functional member", hover)
		self.assertNotIn("main.luau#", hover)

		library = at("textDocument/hover", 2, 6)["contents"]["value"]
		self.assertNotIn("main.luau#", library)
		self.assertNotIn("]==]", library)

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
