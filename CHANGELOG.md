# Changelog

## [Unreleased]

### Added

- Inlay hints for variable types, parameter names, parameter types and return types, each with its own setting
- Documentation in hover, completion and signature help from the Roblox and Luau API docs and Moonwave comments
- Auto-imports of workspace modules and Roblox services in completion
- Semantic highlighting, quick fixes for lints and unknown globals, a `Color3` color picker and folding ranges with `--#region`
- `lunaut.documentationFiles` and `lunaut.disabledGlobals`
- luau-lsp settings (`luau-lsp.*`) are read as well, `lunaut.*` wins when both are set
- `LUNAUT_OFFLINE=1` turns off every download

### Changed

- Hover, completion details and signature help use the same format as luau-lsp

## [0.1.0] - 2026-11-06

### Added

- Language server over stdio: diagnostics, hover, completion, go to definition, document symbols, signature help
- Luau FFlags: enabled by default, synced with Roblox Studio, overrides from settings and the command line
- Definition files (`.d.luau`) with load priorities: settings, `--!priority` directive, globs and discovery
- Rojo: sourcemap requires (`script.Parent`, `game:GetService`, `WaitForChild`, `@self`, `@game`), instance types, sourcemap generation
- Roblox API definitions download and cache
- Roblox typing: `Instance.new`, `GetService`, `Clone`, `fromExisting`, `FindFirstChildWhichIsA` and friends, `IsA` refinement, `Enum.*` types
- `.config.luau` configs, `lunaut.baseLuaurc` and `lunaut.analyzeLuaFiles` settings
- Project settings from `.vscode/settings.json`, read by the server too (also by `lunaut analyze`)
- FFlag names and values validated and completed in `lunaut.fflags.override`
- Completion of aliases, folders and modules inside `require("...")`
- Hover shows `const x: T` for locals declared with `const`
- Find all references and rename across files for locals, module functions and table keys
- `lunaut analyze` and `lunaut flags` commands
- VS Code extension, installers, daily Luau update workflow
- Releases are built, tested and published automatically when the version changes

### Changed

- Sync to upstream Luau 0.742
