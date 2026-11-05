# Changelog

## [Unreleased]

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
- `lunaut analyze` and `lunaut flags` commands
- VS Code extension, installers, daily Luau update workflow

### Changed

- Sync to upstream Luau 0.742
