import os
import pathlib
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


if __name__ == "__main__":
	unittest.main()
