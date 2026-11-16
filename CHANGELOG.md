# Changelog

## [Unreleased]

## [0.0.4] - 2026-11-16

### Changed

- Sync to upstream Luau 0.747

### Added

- `end` and `until` are inserted after Enter on a block opener, with a missing `then` or `do`
- Workspace symbols, document highlight, clickable `require` paths and call hierarchy
- Formatting with StyLua, and `{` in a string turns it into an interpolated string
- Quick fixes: sort imports, add type annotation, use string require in Rojo projects
- Diagnostics for the whole workspace with `lunaut.diagnostics.workspace`
- Roblox Studio companion plugin support (compatible with the luau-lsp plugin)
- The VS Code extension keeps `rojo sourcemap --watch` running
- Requests are queued and can be cancelled; diagnostics of dependents are published after the reply
- Configs for Neovim, Helix and Zed in `editors/`

### Fixed

- Build with Luau 0.743 and newer: `Luau::Frontend` now takes a `SolverMode` in its constructor, the old overload is gone

## [0.0.3] - 2026-11-06

### Changed

- Versions count up by 0.0.1 and carry over every ten: 0.0.9 is followed by 0.1.0, 0.9.9 by 1.0.0
- Earlier releases are renumbered: 0.1.0 is now 0.0.1, 0.2.0 is now 0.0.2
- The Release workflow can be started by hand to build any commit under any version

## [0.0.2] - 2026-11-06

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

## [0.0.1] - 2026-11-05

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
