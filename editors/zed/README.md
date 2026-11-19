# Lunaut for Zed

Runs the [lunaut](https://github.com/TheExeDEV/lunaut) Luau language server in Zed. Highlighting comes from the **Luau** extension, so install both.

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
	},
	"languages": {
		"Luau": { "language_servers": ["lunaut", "!luau-lsp", "..."] }
	}
}
```

The server also reads `.vscode/settings.json` in the project root, so the same project settings work in VS Code and Zed.

## Developing

Zed → Extensions → **Install Dev Extension** → pick this folder.
