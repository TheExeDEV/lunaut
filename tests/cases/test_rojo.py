import json
import pathlib
import shutil
import tempfile
import unittest

from lsp_client import FIXTURES, LspClient, position_of, run_cli


class RojoTests(unittest.TestCase):
	@classmethod
	def setUpClass(cls):
		cls.client = LspClient(FIXTURES / "rojo")
		cls.path = "src/server/Main.server.luau"
		cls.text = (FIXTURES / "rojo" / cls.path).read_text(encoding="utf-8")
		cls.uri, cls.diagnostics = cls.client.open(cls.path)

	@classmethod
	def tearDownClass(cls):
		cls.client.close()

	def test_status(self):
		workspace = self.client.request("lunaut/status", {})["workspaces"][0]
		self.assertEqual(workspace["platform"], "roblox")
		self.assertTrue(workspace["sourcemap"]["loaded"])
		self.assertGreaterEqual(workspace["sourcemap"]["nodes"], 7)

	def test_instance_requires_resolve(self):
		self.assertEqual(self.diagnostics, [])

	def test_required_types_flow(self):
		broken = self.text.replace("local total: number", "local total: string")
		diagnostics = self.client.change(self.uri, broken)
		self.assertTrue(any(diagnostic["code"] == "TypeError" for diagnostic in diagnostics), diagnostics)
		self.assertEqual(self.client.change(self.uri, self.text, version=3), [])

	def test_hover_on_required_module(self):
		line, character = position_of(self.text, "Util.double", 6)
		hover = self.client.at("textDocument/hover", self.uri, line, character)
		self.assertIn("double(value: number): number", hover["contents"]["value"])

	def test_script_instance_type(self):
		line, character = position_of(self.text, "script.Parent", 2)
		hover = self.client.at("textDocument/hover", self.uri, line, character)
		self.assertIn("type script = Script", hover["contents"]["value"])

	def test_completion_from_sourcemap_children(self):
		text = "local parent = script.Parent.\nlocal shared = game:GetService(\"ReplicatedStorage\").Shared.\n"
		self.client.change(self.uri, text, version=10)

		try:
			first = {
				item["label"] for item in self.client.at("textDocument/completion", self.uri, 0, len("local parent = script.Parent."))["items"]
			}
			second = {
				item["label"]
				for item in self.client.at(
					"textDocument/completion", self.uri, 1, len("local shared = game:GetService(\"ReplicatedStorage\").Shared.")
				)["items"]
			}
		finally:
			self.client.change(self.uri, self.text, version=11)

		self.assertTrue({"Helper", "Main"} <= first, first)
		self.assertIn("Util", second)

	def test_roblox_magic_functions(self):
		_, diagnostics = self.client.open("src/server/Magic.server.luau")
		self.assertEqual(diagnostics, [])

		_, diagnostics = self.client.open("src/server/BadMagic.server.luau")
		self.assertEqual(len(diagnostics), 1, diagnostics)
		self.assertIn("Invalid class name 'Nope'", diagnostics[0]["message"])

	def test_roblox_magic_new_solver(self):
		files = ["src/server/Magic.server.luau", "src/server/BadMagic.server.luau"]
		result = run_cli("analyze", *files, "--flag:LuauSolverV2=true", cwd=FIXTURES / "rojo")
		self.assertIn("2 files, 1 errors", result.stdout, result.stdout + result.stderr)
		self.assertIn("Invalid class name 'Nope'", result.stdout)

	def test_user_definitions_see_roblox_enums(self):
		folder = tempfile.mkdtemp()
		self.addCleanup(shutil.rmtree, folder, True)
		definition = pathlib.Path(folder, "enums.d.luau")
		definition.write_text(
			"declare function pick_material(material: Enum.Material | string): Enum.Material\n",
			encoding="utf-8",
		)
		user = pathlib.Path(folder, "user.luau")
		user.write_text("--!strict\nlocal result: Enum.Material = pick_material(Enum.Material.Wood)\nreturn result\n", encoding="utf-8")

		for flags in ([], ["--flag:LuauSolverV2=true"]):
			result = run_cli("analyze", f"--definition=@enums={definition}", *flags, str(user), cwd=FIXTURES / "rojo")
			self.assertNotIn("Unknown type", result.stdout + result.stderr)
			self.assertIn("0 errors", result.stdout, result.stdout + result.stderr)

	def test_auto_imports_from_rojo_packages(self):
		folder = tempfile.mkdtemp()
		self.addCleanup(shutil.rmtree, folder, True)
		root = pathlib.Path(folder)

		def write(path, text):
			file = root / path
			file.parent.mkdir(parents=True, exist_ok=True)
			file.write_text(text, encoding="utf-8")

		write("default.project.json", json.dumps({"name": "Game", "tree": {"$className": "DataModel", "ReplicatedStorage": {"Packages": {"$path": "Packages"}, "Shared": {"$path": "src"}}}}))
		write("Packages/Fusion.lua", "return require(script.Parent._Index.fusion.fusion)\n")
		write("Packages/_Index/fusion/fusion/init.lua", "return { New = 1 }\n")
		write("src/Main.luau", "local fusion = Fus\nlocal index = fusi\nlocal storage = ReplicatedSt\nreturn { fusion, index, storage }\n")
		sourcemap = {
			"name": "Game",
			"className": "DataModel",
			"filePaths": ["default.project.json"],
			"children": [
				{
					"name": "ReplicatedStorage",
					"className": "ReplicatedStorage",
					"children": [
						{
							"name": "Packages",
							"className": "Folder",
							"filePaths": ["Packages"],
							"children": [
								{"name": "Fusion", "className": "ModuleScript", "filePaths": ["Packages/Fusion.lua"]},
								{
									"name": "_Index",
									"className": "Folder",
									"filePaths": ["Packages/_Index"],
									"children": [
										{
											"name": "fusion",
											"className": "Folder",
											"children": [{"name": "fusion", "className": "ModuleScript", "filePaths": ["Packages/_Index/fusion/fusion/init.lua"]}],
										}
									],
								},
							],
						},
						{"name": "Shared", "className": "Folder", "filePaths": ["src"], "children": [{"name": "Main", "className": "ModuleScript", "filePaths": ["src/Main.luau"]}]},
					],
				}
			],
		}
		write("sourcemap.json", json.dumps(sourcemap))
		settings = {
			"lunaut.platform": "roblox",
			"lunaut.fflags.sync": False,
			"lunaut.roblox.downloadTypes": False,
			"lunaut.rojo.autogenerate": False,
			"lunaut.ignoreGlobs": ["Packages/**"],
			"lunaut.definitions": [{"path": str(FIXTURES / "rojo" / "types" / "roblox.d.luau"), "name": "roblox", "priority": 1000}],
		}
		client = LspClient(root, {"vscode": settings})
		self.addCleanup(client.close)
		uri, _ = client.open("src/Main.luau")

		def items(line, character):
			result = client.request("textDocument/completion", {"textDocument": {"uri": uri}, "position": {"line": line, "character": character}})
			return {item["label"]: item for item in (result["items"] if isinstance(result, dict) else result) if item.get("additionalTextEdits")}

		fusion = items(0, 17)["Fusion"]
		self.assertEqual(fusion["kind"], 9)
		self.assertIn('require(game:GetService("ReplicatedStorage").Packages.Fusion)', fusion["additionalTextEdits"][0]["newText"])
		self.assertNotIn("fusion", items(1, 18))
		self.assertEqual(items(2, 30)["ReplicatedStorage"]["kind"], 7)

	def test_crater_packages_offer_their_root_module(self):
		folder = tempfile.mkdtemp()
		self.addCleanup(shutil.rmtree, folder, True)
		root = pathlib.Path(folder)

		def write(path, text):
			file = root / path
			file.parent.mkdir(parents=True, exist_ok=True)
			file.write_text(text, encoding="utf-8")

		write("default.project.json", json.dumps({"name": "Game", "tree": {"$className": "DataModel", "ReplicatedStorage": {"Packages": {"$path": "Packages"}, "Shared": {"$path": "src"}}}}))
		write("Packages/vide/.crater-package", "")
		write("Packages/vide/init.luau", "return { mount = 1 }\n")
		write("Packages/vide/action.luau", "return 1\n")
		write("Packages/library/.crater-package", "")
		write("Packages/library/src/Thing.luau", "return 1\n")
		write("src/Main.luau", "local a = vid\nlocal b = actio\nlocal c = Thin\nreturn { a, b, c }\n")
		module = lambda name, path: {"name": name, "className": "ModuleScript", "filePaths": [path]}
		sourcemap = {
			"name": "Game",
			"className": "DataModel",
			"children": [
				{
					"name": "ReplicatedStorage",
					"className": "ReplicatedStorage",
					"children": [
						{
							"name": "Packages",
							"className": "Folder",
							"children": [
								dict(module("vide", "Packages/vide/init.luau"), children=[module("action", "Packages/vide/action.luau")]),
								{"name": "library", "className": "Folder", "children": [{"name": "src", "className": "Folder", "children": [module("Thing", "Packages/library/src/Thing.luau")]}]},
							],
						},
						{"name": "Shared", "className": "Folder", "children": [module("Main", "src/Main.luau")]},
					],
				}
			],
		}
		write("sourcemap.json", json.dumps(sourcemap))
		base = {
			"lunaut.platform": "roblox",
			"lunaut.fflags.sync": False,
			"lunaut.roblox.downloadTypes": False,
			"lunaut.rojo.autogenerate": False,
			"lunaut.ignoreGlobs": ["Packages/**"],
		}

		def labels(settings):
			client = LspClient(root, {"vscode": {**base, **settings}})

			try:
				uri, _ = client.open("src/Main.luau")
				found = set()

				for line, character in ((0, 13), (1, 15), (2, 14)):
					result = client.request("textDocument/completion", {"textDocument": {"uri": uri}, "position": {"line": line, "character": character}})
					found |= {item["label"] for item in (result["items"] if isinstance(result, dict) else result) if item.get("additionalTextEdits")}

				return found
			finally:
				client.close()

		offered = labels({})
		self.assertIn("vide", offered)
		self.assertIn("Thing", offered)
		self.assertNotIn("action", offered)
		self.assertIn("action", labels({"lunaut.completion.imports.packageInternals": True}))

	def test_references_through_sourcemap(self):
		line, character = position_of(self.text, "Util.double", 6)
		references = self.client.at("textDocument/references", self.uri, line, character)
		files = sorted(location["uri"].rsplit("/", 1)[1] for location in references)
		self.assertEqual(files, ["Main.server.luau", "Util.luau"])

	def test_definitions_loaded(self):
		listed = self.client.request("lunaut/definitions", {})[0]["definitions"]
		self.assertTrue(all(definition["success"] for definition in listed), listed)

	def test_definition_into_sourcemap_module(self):
		line, character = position_of(self.text, "Util.double", 6)
		location = self.client.at("textDocument/definition", self.uri, line, character)
		self.assertTrue(location["uri"].endswith("Util.luau"), location)


if __name__ == "__main__":
	unittest.main()
