import json
import os
import pathlib
import stat
import tempfile
import unittest

from lsp_client import LspClient, run_cli

DECLARE = ".lunaut-declare"
BAD_CODE = '--!strict\nlocal value: number = "text"\nreturn value\n'


def read_declare(root):
	return json.loads((pathlib.Path(root) / DECLARE).read_text(encoding="utf-8"))


def write_file(path, text):
	path.parent.mkdir(parents=True, exist_ok=True)
	path.write_text(text, encoding="utf-8")


def get_reported_files(folder):
	output = run_cli("analyze", cwd=folder).stdout.replace("\\", "/")
	return {line.split(":", 1)[0] for line in output.splitlines() if ": error [" in line}


def get_version():
	return run_cli("version").stdout.split()[1]


class DeclareTests(unittest.TestCase):
	def test_declare_prints_the_declaration(self):
		result = run_cli("declare", "--json")
		self.assertEqual(result.returncode, 0, result.stderr)
		declared = json.loads(result.stdout)

		self.assertEqual(declared["api_version"], 1)
		self.assertEqual(declared["lunaut_version"], get_version())
		self.assertEqual(json.loads(run_cli("declare").stdout), declared)

		for feature in ("definitions", "definition_priority", "show_as_module", "base_luaurc", "rojo", "fflags", "analyzer"):
			self.assertIsInstance(declared["features"][feature], int, feature)

		settings = declared["settings"]
		self.assertEqual(settings["file"], ".vscode/settings.json")
		self.assertEqual(settings["prefix"], "lunaut.")
		self.assertEqual(settings["precedence"], "lunaut")
		self.assertEqual(settings["compatible"]["luau-lsp.types.definitionFiles"], "lunaut.definitions")
		self.assertEqual(settings["compatible"]["luau-lsp.ignoreGlobs"], "lunaut.ignoreGlobs")
		self.assertEqual(settings["compatible"]["luau-lsp.server.baseLuaurc"], "lunaut.baseLuaurc")
		self.assertTrue(all(key.startswith("luau-lsp.") and value.startswith("lunaut.") for key, value in settings["compatible"].items()))

		definitions = declared["definitions"]
		self.assertEqual(definitions["setting"], "lunaut.definitions")
		self.assertEqual(definitions["compatible_setting"], "luau-lsp.types.definitionFiles")
		self.assertFalse(definitions["merge_with_compatible"])
		self.assertEqual(set(definitions["forms"]), {"array", "string", "object"})
		self.assertEqual(definitions["entry"]["priority"]["default"], 0)
		self.assertEqual(definitions["entry"]["show_as_module"]["default"], False)
		self.assertEqual(definitions["priority_sources"], ["settings", "directive", "default"])
		self.assertEqual(definitions["priority_directive"]["text"], "--!priority")
		self.assertIn("@roblox", definitions["reserved_names"])

		self.assertEqual(declared["cli"]["json_output"]["stats"], 1)
		self.assertIn("settings", declared["schemas"])

	def test_init_writes_a_hidden_file(self):
		with tempfile.TemporaryDirectory() as folder:
			result = run_cli("init", folder)
			self.assertEqual(result.returncode, 0, result.stderr)
			self.assertIn("wrote", result.stdout)
			self.assertEqual(read_declare(folder), json.loads(run_cli("declare", "--json").stdout))

			if os.name == "nt":
				attributes = os.stat(pathlib.Path(folder) / DECLARE).st_file_attributes
				self.assertTrue(attributes & stat.FILE_ATTRIBUTE_HIDDEN)

			again = run_cli("init", folder)
			self.assertEqual(again.returncode, 0, again.stderr)
			self.assertIn("up to date", again.stdout)

	def test_init_adds_missing_keys_and_keeps_old_ones(self):
		with tempfile.TemporaryDirectory() as folder:
			old = {
				"api_version": 1,
				"lunaut_version": "0.0.1",
				"features": {"definitions": 1},
				"settings": {"prefix": "lunaut.", "compatible": {"luau-lsp.custom": "lunaut.custom"}},
				"tool_note": {"keep": True},
			}
			write_file(pathlib.Path(folder) / DECLARE, json.dumps(old))

			result = run_cli("init", folder)
			self.assertEqual(result.returncode, 0, result.stderr)
			self.assertIn("added the missing keys", result.stdout)

			declared = read_declare(folder)
			self.assertEqual(declared["tool_note"], {"keep": True})
			self.assertEqual(declared["settings"]["compatible"]["luau-lsp.custom"], "lunaut.custom")
			self.assertEqual(declared["settings"]["compatible"]["luau-lsp.types.definitionFiles"], "lunaut.definitions")
			self.assertEqual(declared["lunaut_version"], get_version())
			self.assertIn("rojo", declared["features"])
			self.assertIn("definitions", declared)
			self.assertIn("cli", declared)

	def test_server_adds_missing_keys_on_start(self):
		with tempfile.TemporaryDirectory() as folder:
			root = pathlib.Path(folder)
			write_file(root / "main.luau", "return 1\n")
			write_file(root / DECLARE, json.dumps({"api_version": 1, "features": {"rojo": 1}, "tool_note": "keep"}))
			client = LspClient(root, {"fflags": {"sync": False}})

			try:
				client.open("main.luau")
			finally:
				client.close()

			declared = read_declare(root)
			self.assertEqual(declared["tool_note"], "keep")
			self.assertIn("definitions", declared)
			self.assertIn("compatible", declared["settings"])
			self.assertEqual(declared["lunaut_version"], get_version())

	def test_server_leaves_a_complete_file_alone(self):
		with tempfile.TemporaryDirectory() as folder:
			root = pathlib.Path(folder)
			write_file(root / "main.luau", "return 1\n")
			run_cli("init", folder)
			before = (root / DECLARE).read_bytes()
			modified = os.stat(root / DECLARE).st_mtime_ns
			client = LspClient(root, {"fflags": {"sync": False}})

			try:
				client.open("main.luau")
			finally:
				client.close()

			self.assertEqual((root / DECLARE).read_bytes(), before)
			self.assertEqual(os.stat(root / DECLARE).st_mtime_ns, modified)

	def test_server_without_the_file_does_not_create_it(self):
		with tempfile.TemporaryDirectory() as folder:
			root = pathlib.Path(folder)
			write_file(root / "main.luau", "return 1\n")
			client = LspClient(root, {"fflags": {"sync": False}})

			try:
				client.open("main.luau")
			finally:
				client.close()

			self.assertFalse((root / DECLARE).exists())

	def test_crater_package_folders_are_ignored(self):
		with tempfile.TemporaryDirectory() as folder:
			root = pathlib.Path(folder)
			crater_declare = {
				"path_to_crater_settings": "crater.settings.json",
				"sync": {"tool": "azul", "directory": "sync", "packages": {"Packages": "ReplicatedStorage.Packages"}},
				"vendor": {"folders": {"client": {"path": "_vendor/client", "alias": "client"}}},
			}
			write_file(root / ".crater-declare", json.dumps(crater_declare))
			write_file(root / "crater.settings.json", json.dumps({"server": {"path": "Server/Packages", "alias": ""}, "develop": ""}))

			for path in ("Packages/bad.luau", "_vendor/client/bad.luau", "Server/Packages/bad.luau", "ServerPackages/bad.luau", "DevPackages/bad.luau", "src/main.luau"):
				write_file(root / path, BAD_CODE)

			self.assertEqual(get_reported_files(folder), {"src/main.luau", "ServerPackages/bad.luau", "DevPackages/bad.luau"})

	def test_without_crater_nothing_is_ignored(self):
		with tempfile.TemporaryDirectory() as folder:
			root = pathlib.Path(folder)
			write_file(root / "Packages/bad.luau", BAD_CODE)

			self.assertEqual(get_reported_files(folder), {"Packages/bad.luau"})

	def test_definitions_accept_a_single_path(self):
		with tempfile.TemporaryDirectory() as folder:
			root = pathlib.Path(folder)
			write_file(root / "types.d.luau", "declare function greet(name: string): string\n")
			write_file(root / ".vscode/settings.json", json.dumps({"lunaut.definitions": "types.d.luau"}))
			write_file(root / "main.luau", '--!strict\nlocal text: string = greet("you")\nreturn text\n')

			result = run_cli("analyze", "main.luau", cwd=folder)
			self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
	unittest.main()
