# Changelog

## [Unreleased]

### Fixed

- Method signatures no longer show empty generic lists such as `update<, , ...>`: unnamed generics are named `a`, `b`, ... and generics that only belong to `self` are hidden
- `game:GetService("Players")`, `part:Clone()` and `FindFirstChildWhichIsA` show the service or class type in hover and inlay hints instead of `Instance`
- A local narrowed by `if x == Enum.Item` checks keeps its declared type in hover and inlay hints, and every use of it gets the same semantic color
- A function returning a nested table such as `dummies.table` shows `{}` instead of the alias that contains the table

### Changed

- The type display code is split into `Core/TypeDisplay`, `TypeAliases`, `TypeComparer` and `TypeLookup`, and `textDocument/typeDefinition` lives in its own feature file
- clang-format no longer puts a return type on its own line

## [0.0.5] - 2026-11-18

### Added

- Go to declaration, type definition and implementation
- Go to definition for globals, extern type members and type names from `.d.luau` files
- `N references` code lenses above functions (`lunaut.codeLens.references`)
- Selection range, inline values for debuggers, monikers and type hierarchy for extern types
- Comments from any `.luau` file, including `.d.luau` definitions, in hover, completion and signature help, for functions, type declarations, intersections and table fields
- Completion items carry the comment above the field or function they insert
- Go to definition on type references, and the name in `type Name = ...` goes to the type declaration from the analyzer, not to its body
- Hover keeps the analyzer's alias name for named table types such as `LuaObject` or `Scheduler` instead of expanding the table
- Function hover keeps the annotated return alias, so `Line.new` is shown as `function library.new(wrap: string?): line` instead of an expanded, cyclic table
- Hover links named types to their definition, and `textDocument/typeDefinition` navigates to the alias declaration
- Signature help takes `@param` descriptions from the comment above the function
- Type statistics over `lunaut/typeStats` and `lunaut stats [paths] [--json]`: totals, origins, kinds, hierarchy and aliases
- Analyzer plugins: a versioned C ABI (`src/Lunaut/analyzer.h`) loads a custom Luau fork at runtime, supports the pre-0.743 and post-0.743 frontend APIs and reports incompatible plugins instead of crashing
- `lunaut.analyzer.enabled`, `lunaut.analyzer.path` and `lunaut.analyzer.diagnostics`
- `lunaut.completion.importKeyword`: insert auto-imports with `local` or `const`
- `lunaut.typeDisplay.aliases` and `lunaut.typeDisplay.refine`: turn off alias names or display simplification to get the analyzer's raw expanded types
- `lunaut.typeDisplay.unknownError`: display `*error-type*` as `unknown` in hovers and type hints, enabled by default
- `lunaut.inlayHints.autoRequire`: when an insertable hint refers to a type from another module, insert the missing `require` together with the `Module.type` annotation
- `lunaut.diagnostics.delay`: wait this many milliseconds after the last change before checking and publishing diagnostics, `1000` by default
- `"show_as_module": true` on a `lunaut.definitions` entry: display its types as if they came from a required module, for example `core.pointer`; the former `@core.pointer` spelling is still accepted in annotations

### Changed

- Syntax highlighting, brackets and indentation come from the luau-lsp grammar, so files look the same as with luau-lsp, and ```` ```luau ```` blocks in hovers are highlighted

### Fixed

- Malformed LSP params and exceptions while checking or publishing diagnostics are caught, so the server keeps running
- Hover on a table field shows `name: type` and its comment
- Hover on a value whose type is an intersection alias shows the alias name and its comment, including aliases from `.d.luau`
- Required modules keep the alias name from the module's annotated return (`service`, `scheduler`, `basis`, `obbvolume`, `luaobject`, ...) instead of an expanded or cyclic type, in both hover and inlay hints
- Annotated returns of generic aliases with defaults (`enumitem<T = enumobject?>`, `wrapper<T = number>`) keep the alias name too
- Loop variables over a function that returns a generic alias (`enumitem`, `marked`, ...) keep the alias name instead of an expanded, mutually recursive table
- Hover and inlay hints keep alias names for optional returns (`scheduler?`, `enumitem?`), local non-exported aliases (`net_event`), inline type arguments (`as<<scheduler>>`), casts and union or intersection values, instead of expanding the structural type
- Alias lookup prefers aliases from the type's own module, so structurally identical aliases from other files no longer win, while `textDocument/typeDefinition` still points at the right declaration
- Anonymous table annotations are shown as written (`{ iteminstance }`, `{ [iteminstance]: pointer }`) instead of a structural type with `tN` placeholders, in hover and inlay hints
- Alias lookup memoizes structural comparisons and filters candidates with a cheap shallow check, so inlay hints and hover on large files no longer spend seconds per variable
- Insertable inlay hints write visible raw table annotations (`{ iteminstance }`, `{ [string]: Service.scheduler }`) verbatim and skip aliases that are private to another module
- Definition files are stored with absolute paths, so hover links resolve for files opened outside the workspace root
- Reading input no longer flushes standard output concurrently with JSON-RPC writes, which corrupted frames and hung shutdown
- A union of the same alias represented as an intersection and as its merged table (for example `enumitem_shared_net_variant` from `variant or SharedNetVariant.Safe`) keeps the alias name instead of `alias | { ... }`
- Function types containing a variadic tail no longer render a doubled ellipsis such as `(......any)`
- Inlay return hints and function hovers render variadic tails through the alias-aware renderer, so `FuncDB:write` shows `...tval` instead of a structural `...argument | string | t1 where t1 = { ... }`
- Hover Definitions include every alias resolved while rendering, so a `const result = {}` inferred as `{observer_var}`, a `work_list | string` union, an intersection and `class.type` all link to their aliases
- Function hovers and function values add a Definition for the function itself
- Go to definition on a required module's function or method prefers its implementation (`Scheduler:whose()` goes to `Scheduler.luau`) and falls back to the type declaration, including intersection-typed owners
- Hovering a reserved word such as `local`, `const`, `if`, `return` or `end` shows nothing instead of a type
- Hovering the `function` keyword shows the normalized function type, with parameter and return aliases such as `line | string` instead of expanded structural tables
- Function hover normalizes parameter and return aliases the same way as locals, and hides inferred variadic tails that have no source `...`
- Primitive-like types never take a structural alias, so `any` no longer displays as `AdReward` and `string` as `ContentId`
- Defaulted generic aliases are compared through their defaults, so union aliases such as `observer_var` stay named instead of expanding to `(...) -> () | observer | thread`
- Inlay hints with a negated type are not offered as an insertable annotation, because `~` is not valid annotation syntax
- `show_as_module` annotations accept `Module.Type` and `@Module.Type`, and resolve to the same type as the bare global name
- Changes are debounced and changed modules are checked in one queued batch on up to eight threads, so an edit no longer rechecks the whole module synchronously for every keystroke

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
