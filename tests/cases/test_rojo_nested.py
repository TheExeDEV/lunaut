import json
import os
import pathlib
import shutil
import sys
import tempfile
import unittest

from lsp_client import FIXTURES, LspClient


def make_fake_rojo(directory, log):
	script = pathlib.Path(directory) / "fake_rojo.py"
	script.write_text(
		"import json, sys\n"
		"arguments = sys.argv[1:]\n"
		f"open({str(log)!r}, 'w').write(arguments[1])\n"
		"output = arguments[arguments.index('--output') + 1]\n"
		"json.dump({'name': 'nested', 'className': 'DataModel', 'children': []}, open(output, 'w'))\n",
		encoding="utf-8",
	)

	if os.name == "nt":
		wrapper = pathlib.Path(directory) / "fake-rojo.cmd"
		wrapper.write_text(f'@"{sys.executable}" "{script}" %*\r\n', encoding="utf-8")
	else:
		wrapper = pathlib.Path(directory) / "fake-rojo"
		wrapper.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{script}" "$@"\n', encoding="utf-8")
		wrapper.chmod(0o755)

	return str(wrapper)


def find_rojo():
	candidates = [os.environ.get("LUNAUT_TEST_ROJO", "")]
	candidates += [str(path) for path in sorted(pathlib.Path.home().glob(".rokit/tool-storage/rojo-rbx/rojo/*/rojo*"))]
	candidates += [shutil.which("rojo") or ""]
	return next((candidate for candidate in candidates if candidate and pathlib.Path(candidate).is_file()), None)


def write(root, path, data):
	file = pathlib.Path(root) / path
	file.parent.mkdir(parents=True, exist_ok=True)
	file.write_text(data if isinstance(data, str) else json.dumps(data), encoding="utf-8")


def make_projects(root):
	write(root, "test.project.json", {"name": "tests", "tree": {"$className": "DataModel", "ReplicatedStorage": {"core": {"$path": "src/core"}}}})
	write(root, "src/default.project.json", {"name": "template", "tree": {"$className": "DataModel", "$path": "src"}})
	write(root, "bin/build.project.json", {"name": "build", "tree": {"$className": "DataModel", "ReplicatedStorage": {"core": {"$path": "core"}}}})
	write(root, "src/core/init.luau", "return {}\n")
	write(root, "bin/core/init.luau", "return {}\n")


class NestedRojoTests(unittest.TestCase):
	def test_nested_project_enables_roblox(self):
		client = LspClient(FIXTURES / "rojo-nested")

		try:
			workspace = client.request("lunaut/status", {})["workspaces"][0]
			self.assertEqual(workspace["platform"], "roblox")
		finally:
			client.close()

	@unittest.skipIf(os.name == "nt", "a .cmd rojo hangs the server on Windows, see the process launch issue")
	def test_sourcemap_uses_nested_project(self):
		with tempfile.TemporaryDirectory() as tools:
			log = pathlib.Path(tools) / "project.txt"
			rojo = make_fake_rojo(tools, log)
			sourcemap = FIXTURES / "rojo-nested" / "sourcemap.json"
			client = LspClient(FIXTURES / "rojo-nested", {"vscode": {"lunaut.rojo.autogenerate": True, "lunaut.rojo.executable": rojo}})

			try:
				workspace = client.request("lunaut/status", {})["workspaces"][0]
				self.assertEqual(log.read_text(encoding="utf-8"), "game/default.project.json")
				self.assertTrue(workspace["sourcemap"]["loaded"])
			finally:
				client.close()
				sourcemap.unlink(missing_ok=True)


	def test_root_project_wins_and_broken_projects_are_marked(self):
		with tempfile.TemporaryDirectory() as root:
			make_projects(root)
			client = LspClient(root, {"vscode": {"lunaut.rojo.autogenerate": False, "lunaut.fflags.sync": False}})

			try:
				folder = client.request("lunaut/rojoProjects", {})[0]
				usable = {project["path"]: project["usable"] for project in folder["projects"]}
				self.assertEqual(folder["selected"], "test.project.json")
				self.assertEqual(usable, {"bin/build.project.json": True, "src/default.project.json": False, "test.project.json": True})
			finally:
				client.close()

	def test_broken_project_explains_the_path(self):
		with tempfile.TemporaryDirectory() as root:
			make_projects(root)
			settings = {"lunaut.rojo.autogenerate": False, "lunaut.fflags.sync": False, "lunaut.rojo.projectFile": "src/default.project.json"}
			client = LspClient(root, {"vscode": settings})

			try:
				result = client.request("lunaut/sourcemap", {"regenerate": True})[0]
				self.assertFalse(result["success"])
				self.assertIn("src/src", result["message"])
			finally:
				client.close()

	@unittest.skipIf(find_rojo() is None, "rojo is not installed")
	def test_nested_project_paths_start_at_the_root(self):
		with tempfile.TemporaryDirectory() as root:
			make_projects(root)
			settings = {"lunaut.rojo.autogenerate": True, "lunaut.fflags.sync": False, "lunaut.rojo.projectFile": "bin/build.project.json", "lunaut.rojo.executable": find_rojo()}
			client = LspClient(root, {"vscode": settings})

			try:
				result = client.request("lunaut/sourcemap", {"regenerate": True})[0]
				self.assertTrue(result["success"], result)
				sourcemap = json.loads((pathlib.Path(root) / "sourcemap.json").read_text(encoding="utf-8"))
				self.assertEqual(sourcemap["filePaths"], ["bin/build.project.json"])
				core = sourcemap["children"][0]["children"][0]
				self.assertEqual(core["filePaths"], ["bin/core/init.luau"])
			finally:
				client.close()


if __name__ == "__main__":
	unittest.main()
