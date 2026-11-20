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
