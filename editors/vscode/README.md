# Lunaut for VS Code

Luau language support powered by [lunaut](https://github.com/TheExeDEV/lunaut), a Luau language server written in C++.

- Type errors, syntax errors and lint while you type
- Hover, completion (including `require` paths and `.luaurc` aliases), go to definition, document symbols, signature help
- Find all references and rename (F2) across files
- Luau FFlags enabled by default and synced with Roblox Studio
- `.d.luau` definition files with load priorities
- Rojo projects: requires through the sourcemap, `script` and services typed from the instance tree

## Commands

- `Lunaut: Restart Server`
- `Lunaut: Show Status`
- `Lunaut: Show FFlags`
- `Lunaut: Show Loaded Definitions`
- `Lunaut: Regenerate Rojo Sourcemap`

## Settings

All settings are under `lunaut.*`, see the [main README](https://github.com/TheExeDEV/lunaut#settings). Project settings go in `.vscode/settings.json`; the server reads that file too, so `lunaut analyze` in CI uses the same settings. Flag names complete inside `lunaut.fflags.override`.
