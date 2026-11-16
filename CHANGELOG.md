# Changelog

## [Unreleased]

### Added

- Comments from any `.luau` file, including `.d.luau` definitions, in hover, completion and signature help, for functions, type declarations, intersections and table fields
- Completion items carry the comment above the field or function they insert
- Go to definition on type references, and the name in `type Name = ...` goes to the type declaration from the analyzer, not to its body
- Hover keeps the analyzer's alias name for named table types such as `LuaObject` or `Scheduler` instead of expanding the table
- Type statistics over `lunaut/typeStats` and `lunaut stats [paths] [--json]`: totals, origins, kinds, hierarchy and aliases
- Analyzer plugins: a versioned C ABI (`src/Lunaut/analyzer.h`) loads a custom Luau fork at runtime, supports the pre-0.743 and post-0.743 frontend APIs and reports incompatible plugins instead of crashing
- `lunaut.analyzer.enabled`, `lunaut.analyzer.path` and `lunaut.analyzer.diagnostics`
- `lunaut.completion.importKeyword`: insert auto-imports with `local` or `const`

### Fixed

- Malformed LSP params and exceptions while checking or publishing diagnostics are caught, so the server keeps running
- Hover on a table field shows `name: type` and its comment
- Hover on a value whose type is an intersection alias shows the alias name and its comment, including aliases from `.d.luau`
- Required modules keep the alias name from the module's annotated return (`service`, `scheduler`, `basis`, `obbvolume`, `luaobject`, ...) instead of an expanded or cyclic type, in both hover and inlay hints
- Reading input no longer flushes standard output concurrently with JSON-RPC writes, which corrupted frames and hung shutdown

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
