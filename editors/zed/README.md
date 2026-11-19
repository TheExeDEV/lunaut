# Lunaut for Zed

Luau support for Zed: syntax highlighting, outline, brackets and indentation, plus the [lunaut](https://github.com/TheExeDEV/lunaut) language server. It replaces the **Luau** extension, so keep only one of them installed.

On first start the extension downloads `lunaut` for your platform from the latest release. A `lunaut` on `PATH` is used instead when there is one, and a fixed binary can be set in Zed's settings:

```json
{
	"lsp": {
		"lunaut": {
			"binary": { "path": "C:/tools/lunaut.exe", "arguments": ["lsp"] },
			"initialization_options": {
				"vscode": { "lunaut.inlayHints.variableTypes": true }
			}
		}
	}
}
```

The server also reads `.vscode/settings.json` in the project root, so the same project settings work in VS Code and Zed.

## Commands

Zed extensions cannot add entries to the command palette, so lunaut offers them in the code actions menu (`Ctrl+.`):

| Action | What it does |
| --- | --- |
| Lunaut: Show Status | version, Roblox or standard mode, loaded definitions and the sourcemap |
| Lunaut: Regenerate Rojo Sourcemap | runs `rojo sourcemap` and reports the result |

To restart the server use `editor: restart language server`, and `dev: open language server logs` shows its log.

## Developing

Zed → Extensions → **Install Dev Extension** → pick this folder.
