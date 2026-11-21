import pathlib
import tempfile
import unittest

from lsp_client import FIXTURES, LspClient, run_cli


def get_actions(client, uri):
	return client.request(
		"textDocument/codeAction",
		{
			"textDocument": {"uri": uri},
			"range": {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 0}},
			"context": {"diagnostics": []},
		},
	)


class ServerActionTests(unittest.TestCase):
	def test_actions_are_offered_to_other_editors(self):
		client = LspClient(FIXTURES / "basic", {"fflags": {"sync": False}})

		try:
			uri, _ = client.open("main.luau")
			commands = {action["title"]: action["command"]["command"] for action in get_actions(client, uri) if "command" in action}
			self.assertEqual(commands["Lunaut: Show Status"], "lunaut.server.showStatus")
			self.assertEqual(commands["Lunaut: Regenerate Rojo Sourcemap"], "lunaut.server.regenerateSourcemap")
			self.assertIn("lunaut.server.showStatus", client.capabilities["executeCommandProvider"]["commands"])

			client.request("workspace/executeCommand", {"command": "lunaut.server.showStatus", "arguments": [uri]})
			status = client.wait_for("window/showMessage", lambda params: "solver" in params["message"], timeout=30)
			version = run_cli("version").stdout.split()[1]
			self.assertTrue(status["message"].startswith(f"Lunaut {version} "), status)
			self.assertIn("standard", status["message"])

			client.request("workspace/executeCommand", {"command": "lunaut.server.regenerateSourcemap", "arguments": [uri]})
			result = client.wait_for("window/showMessage", lambda params: "sourcemap" in params["message"], timeout=30)
			self.assertIn("not a Roblox project", result["message"])
		finally:
			client.close()

	def test_reload_and_studio_plugin_commands(self):
		client = LspClient(FIXTURES / "basic", {"fflags": {"sync": False}})

		try:
			uri, _ = client.open("main.luau")
			titles = [action["title"] for action in get_actions(client, uri)]
			self.assertIn("Lunaut: Reload Workspace", titles)
			self.assertIn("Lunaut: Install Roblox Studio Plugin", titles)
			commands = client.capabilities["executeCommandProvider"]["commands"]
			self.assertIn("lunaut.server.reloadWorkspace", commands)
			self.assertIn("lunaut.server.installStudioPlugin", commands)

			client.request("workspace/executeCommand", {"command": "lunaut.server.reloadWorkspace", "arguments": []})
			reloaded = client.wait_for("window/showMessage", lambda params: "reloaded" in params["message"], timeout=60)
			self.assertIn("1 workspace", reloaded["message"])
			hover = client.request("textDocument/hover", {"textDocument": {"uri": uri}, "position": {"line": 1, "character": 7}})
			self.assertIsNotNone(hover)
		finally:
			client.close()

	def test_install_studio_plugin_without_studio_or_network(self):
		with tempfile.TemporaryDirectory() as home:
			environment = {"LOCALAPPDATA": home, "HOME": home}

			client = LspClient(FIXTURES / "basic", {"fflags": {"sync": False}}, env=environment)

			try:
				client.request("workspace/executeCommand", {"command": "lunaut.server.installStudioPlugin", "arguments": []})
				missing = client.wait_for("window/showMessage", lambda params: "Roblox Studio" in params["message"], timeout=30)
				self.assertIn("was not found", missing["message"])
			finally:
				client.close()

			pathlib.Path(home, "Roblox").mkdir()
			pathlib.Path(home, "Documents", "Roblox").mkdir(parents=True)
			client = LspClient(FIXTURES / "basic", {"fflags": {"sync": False}}, env=environment)

			try:
				client.request("workspace/executeCommand", {"command": "lunaut.server.installStudioPlugin", "arguments": []})
				failed = client.wait_for("window/showMessage", lambda params: "Studio plugin" in params["message"], timeout=30)
				self.assertIn("offline", failed["message"])
			finally:
				client.close()

	def test_vscode_extension_gets_no_actions(self):
		client = LspClient(FIXTURES / "basic", {"vscode": {"lunaut.fflags.sync": False}, "client": "lunaut-vscode"})

		try:
			uri, _ = client.open("main.luau")
			titles = [action["title"] for action in get_actions(client, uri)]
			self.assertFalse([title for title in titles if title.startswith("Lunaut:")], titles)
		finally:
			client.close()


if __name__ == "__main__":
	unittest.main()
