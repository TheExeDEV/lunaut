# lunaut

A language server for Luau and Roblox, written in C++ on top of the official Luau analysis library.

lunaut is your astronaut in Luau code: it checks types and lint while you type, completes names, shows types on hover and jumps to definitions. It reads the same `.luaurc` as the Luau CLI, loads `.d.luau` definition files in the order you choose, follows Rojo projects through the sourcemap and keeps Luau FFlags in sync with Roblox Studio.

**The server**

- Diagnostics (type errors, syntax errors, lint), hover, completion, go to definition, document symbols, signature help
- Find all references (`Shift+F12`) and rename (`F2`) across files, for locals and module members
- Inlay hints for variable types, parameter names, parameter and return types, each switchable in settings
- Documentation in hover, completion and signature help: Roblox and Luau API docs and Moonwave comments in your code
- Comments from any `.luau` file, including `.d.luau` definitions, for functions, type declarations, intersections and table fields
- Click a type name to jump to its declaration, or to the type body in `type Name = ...`
- Type statistics: totals, origins, type kinds, subtype relations and the type hierarchy, over LSP and from the CLI
- Analyzer plugins: load a custom Luau fork at runtime, with an explicit report when the plugin is incompatible
- Auto-imports: modules and Roblox services that are not required yet, with the `require` or `GetService` line added for you
- Semantic highlighting, quick fixes, a color picker for `Color3` and folding with `--#region`
- Reads luau-lsp settings (`luau-lsp.*`) as they are, so a project set up for luau-lsp works without changes
- Both Luau type solvers: the old one by default, the new one with `lunaut.fflags.enableNewSolver`
- `require` by string (`./`, `../`, `@self`, aliases from `.luaurc` and `.config.luau`) and by instance (`script.Parent.X`, `game:GetService("X")`, `:WaitForChild("X")`)
- Completion inside `require("...")`: aliases, folders and modules as you type the path
- A base `.luaurc` for the whole project (`lunaut.baseLuaurc`) under every other config
- Diagnostics follow dependents: change a module and every open file that requires it is checked again
- One static executable, no runtime to install

**FFlags**

- Every non-experimental `Luau*` flag is on by default, like in luau-lsp
- Values synced with the current Roblox Studio build, cached for a day, used offline
- Overrides from `.vscode/settings.json` and `--flag:Name=Value`, each value shows where it came from
- Every flag compiled into Luau is picked up by itself, and the VS Code settings schema lists all of them, so flag names complete in `lunaut.fflags.override`

**Definition files**

- Any number of `.d.luau` files, from settings, globs, the command line or found in the workspace
- A load priority for every file: higher loads first, so base types are ready before the files that extend them
- Priority in settings, or right in the file with `--!priority 50`
- Roblox API definitions downloaded for the security level you pick

**Rojo**

- Reads `sourcemap.json`, regenerates it with `rojo sourcemap` when files are added, removed or the project changes
- Gives `script`, `game` and services real instance types with children from the sourcemap
- Works without Rojo installed when a sourcemap is already there

---

## Contents

1. [Installation](#installation)
2. [Quick start](#quick-start)
3. [VS Code](#vs-code)
4. [Other editors](#other-editors)
5. [Settings](#settings)
6. [FFlags](#fflags)
7. [Definition files and priorities](#definition-files-and-priorities)
8. [Analyzer plugins](#analyzer-plugins)
9. [Rojo](#rojo)
10. [Editor features](#editor-features)
11. [References and rename](#references-and-rename)
12. [Requires and .luaurc](#requires-and-luaurc)
13. [Command reference](#command-reference)
14. [Custom LSP methods](#custom-lsp-methods)
15. [Environment variables](#environment-variables)
16. [Tests](#tests)
17. [Luau updates](#luau-updates)
18. [Releasing](#releasing)
19. [Building from source](#building-from-source)
20. [Source layout](#source-layout)
21. [Limitations](#limitations)
22. [Acknowledgements](#acknowledgements)
23. [License](#license)

---

## Installation

One script for every system. It detects Windows, Linux or macOS and the processor (x64 or arm64) by itself.

**Windows** (PowerShell):

```powershell
irm https://raw.githubusercontent.com/TheExeDEV/lunaut/main/installers/install | iex
```

**Linux, macOS, Git Bash:**

```bash
curl -fsSL https://raw.githubusercontent.com/TheExeDEV/lunaut/main/installers/install | sh
```

Both lines run the same file. It downloads the latest release into `%LOCALAPPDATA%\lunaut\bin` (Windows) or `~/.lunaut/bin`, adds that folder to `PATH` and, when the `code` command is available, installs the VS Code extension too. Open a new terminal and run `lunaut --version`.

Options, set before the command: `LUNAUT_VERSION=0.1.0` installs a specific version, `LUNAUT_INSTALL_DIR` changes the folder, `LUNAUT_NO_PATH=1` leaves `PATH` alone, `LUNAUT_NO_EXTENSION=1` skips VS Code. In PowerShell: `$env:LUNAUT_VERSION = "0.1.0"`.

**Manually:** download `lunaut-<platform>` from the latest release, rename it to `lunaut` (or `lunaut.exe`) and put it on your `PATH`. Install `lunaut.vsix` with `code --install-extension lunaut.vsix`.

### Uninstall

Also one script for every system:

```powershell
irm https://raw.githubusercontent.com/TheExeDEV/lunaut/main/installers/uninstall | iex
```

```bash
curl -fsSL https://raw.githubusercontent.com/TheExeDEV/lunaut/main/installers/uninstall | sh
```

It removes the executable, the VS Code extension and the `PATH` entries the installer added. The download cache (synced FFlags, Roblox definitions) is kept; to remove it too:

```powershell
$env:LUNAUT_PURGE = 1; irm https://raw.githubusercontent.com/TheExeDEV/lunaut/main/installers/uninstall | iex
```

```bash
curl -fsSL https://raw.githubusercontent.com/TheExeDEV/lunaut/main/installers/uninstall | sh -s -- --purge
```

---

## Quick start

Check a project from the terminal:

```bash
lunaut analyze src
```

```text
src/main.luau:4:23: error [TypeError] Type 'number' could not be converted into 'string'
src/main.luau:6:16: warning [FunctionUnused] FunctionUnused: Function 'unusedHelper' is never used; prefix with '_' to silence
2 files, 1 errors, 1 warnings
```

The exit code is `1` when there are errors, so it fits CI as is.

Ask where the types come from:

```bash
lunaut stats src
```

```text
Types: 28 total, 1 modules, 3 definition files
  in modules: 21, in globals: 69
  by kind: any 1, extern 5, function 27, never 1, primitive 6, table 11, ...
Origins:
  @base (Config) 3 types types/base.d.luau
  @roblox (Roblox) 210 types globalTypes.d.luau
  src/util (module) 14 internal, 8 interface
```

Open the folder in VS Code with the extension installed and the same diagnostics appear in the editor. For a Roblox project, put a `default.project.json` (or a `sourcemap.json`) in the root: lunaut switches to Roblox mode, downloads the API definitions and follows the instance tree.

---

## VS Code

The extension lives in [`editors/vscode`](editors/vscode). It starts the bundled server, or the one from `lunaut.path`, or `lunaut` from `PATH`.

Commands (`Ctrl+Shift+P`):

| Command | What it does |
| --- | --- |
| `Lunaut: Restart Server` | restarts the server, needed after FFlag changes |
| `Lunaut: Show Status` | version, solver, workspaces, sourcemap, loaded definitions |
| `Lunaut: Show FFlags` | every Luau FFlag with its value and source, with a filter |
| `Lunaut: Show Loaded Definitions` | definition files in load order with priorities and errors |
| `Lunaut: Regenerate Rojo Sourcemap` | runs `rojo sourcemap` and reloads it |

Settings are described in [Settings](#settings). Put project settings in `.vscode/settings.json` and commit it, so everyone on the team and CI get the same ones.

**Local build of the extension:**

```bash
cd editors/vscode
npm install
npm run package
code --install-extension lunaut.vsix
```

`npm run package` copies `build/lunaut(.exe)` into the extension, so build the server first. `npm test` starts a real VS Code with the extension and checks diagnostics, hover, completion and commands.

---

## Other editors

Any editor with LSP support can run `lunaut lsp`. Ready configs for Neovim, Helix and Zed are in [`editors/`](editors/README.md). The server reads `.vscode/settings.json` itself, so a project configured for VS Code works the same everywhere.

---

## Settings

All settings live in VS Code settings under `lunaut.*` (luau-lsp settings work too, see [luau-lsp settings](#luau-lsp-settings)): user settings for your machine, `.vscode/settings.json` for a project. The server reads `.vscode/settings.json` itself too, so `lunaut analyze` in a terminal or CI uses exactly the same project settings as the editor. Comments and trailing commas are fine, like everywhere in VS Code.

```jsonc
// .vscode/settings.json
{
  "lunaut.platform": "auto",
  "lunaut.fflags.enableByDefault": true,
  "lunaut.fflags.sync": true,
  "lunaut.fflags.enableNewSolver": false,
  "lunaut.fflags.override": { "LuauTypeInferIterationLimit": "4000" },
  "lunaut.definitions": [
    { "path": "types/base.d.luau", "priority": 100 },
    { "path": "types/**/*.d.luau", "priority": 10 },
    "vendor/extra.d.luau",
  ],
  "lunaut.discoverDefinitions": true,
  "lunaut.roblox.downloadTypes": true,
  "lunaut.roblox.securityLevel": "PluginSecurity",
  "lunaut.rojo.autogenerate": true,
  "lunaut.ignoreGlobs": ["**/_Index/**"],
  "lunaut.baseLuaurc": "",
}
```

| Setting | Default | Meaning |
| --- | --- | --- |
| `lunaut.path` | `""` | server executable; empty uses the bundled one, then `lunaut` from `PATH`; `~/` works |
| `lunaut.platform` | `auto` | `standard`, `roblox`, or `auto`: Roblox when a Rojo project or sourcemap is in the root |
| `lunaut.fflags.enableByDefault` | `true` | turn on every non-experimental `Luau*` flag |
| `lunaut.fflags.sync` | `true` | take flag values from the current Roblox Studio build |
| `lunaut.fflags.enableNewSolver` | `false` | `LuauSolverV2 = true` |
| `lunaut.fflags.override` | `{}` | flag values that win over everything except `--flag`; flag names complete as you type |
| `lunaut.definitions` | `[]` | definition files, see below |
| `lunaut.discoverDefinitions` | `true` | also load every `*.d.luau` in the workspace |
| `lunaut.roblox.downloadTypes` | `true` | download Roblox API definitions in Roblox mode |
| `lunaut.roblox.securityLevel` | `PluginSecurity` | `None`, `LocalUserSecurity`, `PluginSecurity`, `RobloxScriptSecurity` |
| `lunaut.roblox.typesPriority` | `1000` | load priority of the Roblox definitions |
| `lunaut.rojo.enabled` | `true` | use the sourcemap |
| `lunaut.rojo.projectFile` | `default.project.json` | Rojo project used to generate the sourcemap |
| `lunaut.rojo.sourcemapFile` | `sourcemap.json` | sourcemap to read |
| `lunaut.rojo.autogenerate` | `true` | run `rojo sourcemap` on start and on file changes |
| `lunaut.rojo.includeNonScripts` | `true` | pass `--include-non-scripts`, so folders and models get types |
| `lunaut.rojo.strictDatamodelTypes` | `false` | old solver only: use instance types in diagnostics, not only in hover and completion |
| `lunaut.rojo.executable` | `rojo` | Rojo command |
| `lunaut.diagnostics.lint` | `true` | run Luau lint |
| `lunaut.diagnostics.includeDependents` | `true` | re-check open files that require a changed module |
| `lunaut.ignoreGlobs` | `["**/_Index/**"]` | files that never get diagnostics and are never discovered |
| `lunaut.analyzeLuaFiles` | `true` | check `.lua` files too, not only `.luau` |
| `lunaut.baseLuaurc` | `""` | a `.luaurc` applied under every other one, relative to the workspace |
| `lunaut.inlayHints.variableTypes` | `true` | type hints after locals and loop variables |
| `lunaut.inlayHints.parameterNames` | `literals` | parameter names before arguments: `none`, `literals` or `all` |
| `lunaut.inlayHints.parameterTypes` | `false` | type hints after function parameters without annotations |
| `lunaut.inlayHints.functionReturnTypes` | `true` | return type hints after function signatures |
| `lunaut.inlayHints.hideHintsForErrorTypes` | `true` | skip hints for types that failed to infer |
| `lunaut.inlayHints.hideHintsForMatchingParameterNames` | `true` | skip `value:` before an argument already named `value` |
| `lunaut.inlayHints.typeHintMaxLength` | `50` | longer type hints are cut |
| `lunaut.inlayHints.makeInsertable` | `true` | double-click a type hint to insert it as an annotation |
| `lunaut.hover.documentation` | `true` | documentation in hover, completion and signature help |
| `lunaut.documentationFiles` | `[]` | extra documentation files in the Roblox API docs format |
| `lunaut.disabledGlobals` | `[]` | globals to remove, like `table` or `table.clone` |
| `lunaut.completion.autoImports` | `true` | suggest modules and services that are not required yet |
| `lunaut.semanticTokens.enabled` | `true` | semantic highlighting |
| `lunaut.colors.enabled` | `true` | color picker for `Color3` |
| `lunaut.completion.autocompleteEnd` | `true` | insert `end` or `until` after Enter on a block opener, adding a missing `then` or `do` |
| `lunaut.format.convertQuotes` | `true` | typing `{` in a `"string"` turns it into an interpolated string |
| `lunaut.format.enabled` | `true` | format documents and selections with StyLua |
| `lunaut.format.styluaPath` | `stylua` | StyLua executable |
| `lunaut.diagnostics.workspace` | `false` | diagnostics for every file in the workspace, not only open ones |
| `lunaut.analyzer.enabled` | `false` | load the analyzer plugin from `lunaut.analyzer.path`; setting a path turns it on |
| `lunaut.analyzer.path` | `""` | shared library with a custom Luau analyzer, see [Analyzer plugins](#analyzer-plugins) |
| `lunaut.analyzer.diagnostics` | `false` | merge diagnostics returned by the analyzer plugin with the built-in ones |
| `lunaut.rojo.watch` | `true` | VS Code only: keep `rojo sourcemap --watch` running instead of regenerating on every change |
| `lunaut.studioPlugin.enabled` | `false` | VS Code only: accept the live DataModel from the Studio companion plugin |
| `lunaut.studioPlugin.port` | `3667` | port of the Studio companion plugin |
| `lunaut.trace.server` | `off` | log LSP traffic to the Lunaut output channel |

### luau-lsp settings

lunaut understands the settings of [luau-lsp](https://github.com/JohnnyMorganz/luau-lsp), in `.vscode/settings.json` and in your user settings. When both are set, `lunaut.*` wins.

| luau-lsp | lunaut |
| --- | --- |
| `luau-lsp.platform.type` | `lunaut.platform` |
| `luau-lsp.fflags.enableByDefault`, `sync`, `enableNewSolver`, `override` | `lunaut.fflags.*` |
| `luau-lsp.types.definitionFiles` | `lunaut.definitions` |
| `luau-lsp.types.documentationFiles` | `lunaut.documentationFiles` |
| `luau-lsp.types.disabledGlobals` | `lunaut.disabledGlobals` |
| `luau-lsp.types.robloxSecurityLevel` | `lunaut.roblox.securityLevel` |
| `luau-lsp.types.roblox` | `lunaut.roblox.downloadTypes` |
| `luau-lsp.sourcemap.enabled`, `rojoProjectFile`, `sourcemapFile`, `autogenerate`, `includeNonScripts`, `rojoPath` | `lunaut.rojo.*` |
| `luau-lsp.diagnostics.strictDatamodelTypes` | `lunaut.rojo.strictDatamodelTypes` |
| `luau-lsp.diagnostics.includeDependents` | `lunaut.diagnostics.includeDependents` |
| `luau-lsp.inlayHints.*` | `lunaut.inlayHints.*` |
| `luau-lsp.completion.imports.enabled` | `lunaut.completion.autoImports` |
| `luau-lsp.ignoreGlobs` | `lunaut.ignoreGlobs` |
| `luau-lsp.analyzeLuaFiles` | `lunaut.analyzeLuaFiles` |
| `luau-lsp.server.baseLuaurc` | `lunaut.baseLuaurc` |

The Roblox definitions and API docs are the same files luau-lsp uses, from `luau-lsp.pages.dev`, loaded as the `@roblox` package, so hover, completion and documentation symbols match.

---

## FFlags

FFlags switch features of the Luau analyzer on and off. lunaut sets them once on start, in this order (later wins):

1. **Enable by default.** Every `Luau*` boolean flag that Luau does not mark as experimental becomes `true`.
2. **Sync.** The flags of the current Roblox Studio build are downloaded from `clientsettingscdn.roblox.com` and applied, so you see what Studio sees. The answer is cached for 24 hours in `%LOCALAPPDATA%\lunaut\cache` (`~/.cache/lunaut/cache`); without internet the cached copy is used.
3. **New solver.** `lunaut.fflags.enableNewSolver` sets `LuauSolverV2`.
4. **Override.** `lunaut.fflags.override`.
5. **Command line.** `--flag:Name=Value`.

Names go without the `FFlag`/`FInt` prefix: `LuauSolverV2`, not `FFlagLuauSolverV2`. Booleans take `true`/`false`, integers take numbers. A wrong value or an unknown name is reported in the log and skipped.

See every flag, its value and where it came from:

```bash
lunaut flags --flag:LuauSolverV2=true
```

```text
LuauSolverV2=true  (bool, Cli)
LuauTypeInferIterationLimit=20000  (int, Default)
...
```

In VS Code: `Lunaut: Show FFlags`. Flags apply to the whole server, so changing them asks for a restart.

---

## Definition files and priorities

A definition file (`.d.luau`) declares globals and classes without code:

```luau
--!priority 100
declare extern type Vehicle with
	speed: number
	function drive(self, distance: number): number
end
```

```luau
--!priority 50
declare extern type Car extends Vehicle with
	brand: string
end

declare garage: { Car }
```

Order matters: `Car extends Vehicle` only works if `Vehicle` is already loaded. lunaut sorts every file by priority, **higher first**, and keeps the listed order for equal priorities.

Where a file gets its priority, first match wins:

1. `priority` in settings or `--definition=path#priority`
2. the `--!priority N` line at the top of the file
3. `0`

Where files come from:

| Source | Example |
| --- | --- |
| settings | `"lunaut.definitions": [{ "path": "types/base.d.luau", "priority": 100 }]` |
| settings with a glob | `"lunaut.definitions": [{ "path": "types/**/*.d.luau", "priority": 10 }]` |
| settings as a map | `"lunaut.definitions": { "@roblox": "types/globalTypes.d.luau" }` |
| command line | `--definition=@base=types/base.d.luau#100` |
| discovery | every `*.d.luau` in the workspace when `lunaut.discoverDefinitions` is on |
| Roblox | `globalTypes.<security_level>.d.luau`, priority `lunaut.roblox.typesPriority` |

A file listed twice is loaded once, with the first priority found. Every file gets a package name (`@base`, `@roblox`); set it with `name`. Errors in a definition file show up as diagnostics in that file and in `Lunaut: Show Loaded Definitions`. Changes to definition files need a restart.

---

## Analyzer plugins

lunaut can hand type checking to a custom Luau fork, loaded at runtime as a shared library. This is for projects that patch Luau itself and need the server to use the same analyzer. Set `lunaut.analyzer.path` to the library and lunaut loads it on start and whenever the setting changes:

```json
{
  "lunaut.analyzer.enabled": true,
  "lunaut.analyzer.path": "./tools/libmy-luau-analyzer.so",
  "lunaut.analyzer.diagnostics": true
}
```

The plugin exports three C functions and one versioned info structure, declared in [`src/Lunaut/analyzer.h`](src/Lunaut/analyzer.h):

| Symbol | Purpose |
| --- | --- |
| `lunaut_analyzer_info_v1` | returns `lunaut_analyzer_info`: ABI version, name, Luau version and frontend API |
| `lunaut_analyzer_check_v1` | checks one module and returns a JSON array of LSP diagnostics |
| `lunaut_analyzer_free_v1` | frees the diagnostics string, optional |

The frontend API is `1` for Luau before 0.743 and `2` from 0.743 on. lunaut validates the ABI, the reported Luau version and the API pair; a plugin built for the wrong side of 0.743, an unknown ABI or a missing symbol is not loaded, and the reason is reported in `Lunaut: Show Status` and in the server log instead of crashing the server. `lunaut/status` shows the loaded analyzer under `analyzer` in every workspace.

With `lunaut.analyzer.diagnostics` the plugin diagnostics are merged with the built-in ones. Without it the plugin only reports whether it is loaded and compatible.

---

## Rojo

In Roblox mode lunaut reads `sourcemap.json`. Each script in it gets a virtual name like `game/ReplicatedStorage/Shared/Util`, so these all resolve to the same module:

```luau
local ReplicatedStorage = game:GetService("ReplicatedStorage")
local Util = require(ReplicatedStorage.Shared.Util)
local Util = require(ReplicatedStorage:WaitForChild("Shared"):WaitForChild("Util"))
local Helper = require(script.Parent.Helper)
local Helper = require("./Helper")
local Util = require("@game/ReplicatedStorage/Shared/Util")
local Child = require("@self/Child")
```

`script.Parent`, `FindFirstChild`, `WaitForChild`, `FindFirstAncestor` and `game:GetService` are followed. `Players.LocalPlayer.PlayerScripts` and `PlayerGui` point to `StarterPlayerScripts` and `StarterGui`.

`script`, `game` and every service get instance types with their children from the sourcemap, so `script.Parent.Config.Value` completes. With the old solver these types are used in hover and completion; set `lunaut.rojo.strictDatamodelTypes` to use them in diagnostics too. The new solver always uses them.

**Keeping the sourcemap fresh.** With `lunaut.rojo.autogenerate` lunaut runs

```bash
rojo sourcemap default.project.json --output sourcemap.json --include-non-scripts
```

on start, when a `.luau` file is created or deleted and when a `*.project.json` changes. Without Rojo on `PATH` it uses the `sourcemap.json` that is already there, and reloads it whenever it changes, for example when you run `rojo sourcemap --watch` yourself.

In VS Code the extension keeps `rojo sourcemap --watch` running for every workspace folder with a project file (`lunaut.rojo.watch`), and the server only reloads what Rojo writes. If Rojo exits, the server goes back to regenerating on its own.

### Studio companion plugin

With `lunaut.studioPlugin.enabled` the VS Code extension listens on `127.0.0.1:3667` for the [luau-lsp Studio Companion plugin](https://create.roblox.com/store/asset/10913122509). The plugin sends the live DataModel from Studio, and instances that exist only in the place, not in the Rojo project, get types and completion too. *Clear* in the plugin drops them again. Run luau-lsp or lunaut, not both: they use the same port.

`.server.luau` and `.client.luau` files are checked as scripts, everything else as modules.

### Roblox types

In Roblox mode the Roblox API definitions get the same extra typing as in luau-lsp, in both solvers:

| Code | Type |
| --- | --- |
| `Instance.new("Part")` | `Part`; an unknown or non-creatable class is an error |
| `game:GetService("Players")` | `Players`; an unknown service is an error |
| `part:Clone()` | the type of `part` |
| `Instance.fromExisting(part)` | the type of `part` |
| `x:FindFirstChildWhichIsA("Model")`, `OfClass`, `FindFirstAncestorWhichIsA`, `OfClass` | `Model?` |
| `if x:IsA("BasePart") then` | `x` is a `BasePart` inside the branch |
| `local m: Enum.Material` | enum types work in annotations |

The lists of creatable classes and services come from the metadata line at the top of the downloaded definitions.

---

## Editor features

### Hover

Hover text follows luau-lsp:

| Under the cursor | Hover |
| --- | --- |
| a local | `local count: number`, `const limit: number` |
| a function | `function Util.double(value: number): number`, methods as `function Part:Clone(): Part` |
| a global | `type game = DataModel` |
| a type name | `type Vector<T> = { x: T, y: T }` |
| a table field | `x: number` |
| a string | `string (6 bytes)` |

Documentation follows under a `----------` line: the Roblox or Luau API docs for API members, otherwise the comment right above the declaration. Comments are read from any `.luau` file, including `.d.luau` definitions, and work for functions, type declarations, intersections and table fields. Moonwave comments are rendered with `@param`, `@return`, `@error` sections and `@deprecated`, `@yields`, `@since` labels:

```luau
--[=[
	Doubles a number.

	@param value number to double
	@return the doubled number
]=]
local function double(value: number): number
```

Signature help shows the same documentation for the function and each parameter.

### Inlay hints

Gray hints inside the code: `local result = scale(2, 3)` shows `: number` after `result` and `value:`, `factor:` before the arguments. Every kind has its own switch under `lunaut.inlayHints`; VS Code's `editor.inlayHints.enabled` turns all of them off at once. Changes apply right away, no restart needed.

### Completion and auto-imports

Functions show their parameters next to the name and fill the call with placeholders: `add(${1:a}, ${2:b})`. Names of modules in the workspace and Roblox services that are not imported yet appear too; picking one adds the line at the top, after the existing imports:

| Picked | Added |
| --- | --- |
| a module next to the file | `local Util = require("./Util")` |
| a module under a `.luaurc` alias | `local Signal = require("@packages/Signal")` |
| a sibling instance in Rojo | `local Helper = require(script.Parent.Helper)` |
| another instance in Rojo | `local Util = require(game:GetService("ReplicatedStorage").Shared.Util)` |
| a Roblox service | `local Players = game:GetService("Players")` |

### Quick fixes

| Diagnostic | Fix |
| --- | --- |
| `LocalUnused`, `FunctionUnused`, `ImportUnused` | rename to `_name` |
| any lint | add `--!nolint Name` at the top of the file |
| `Unknown global 'Util'` | add the `require` or `GetService` line |

Without a diagnostic:

| Under the cursor | Action |
| --- | --- |
| anywhere | *Sort imports*: sorts the block of `require` and `GetService` lines at the top |
| a local without an annotation | *Add type annotation*: writes the inferred type, like `: number` |
| `require(script.Parent.Helper)` in Rojo | *Use string require*: `require("./Helper")`, or `require("@game/ReplicatedStorage/Shared/Util")` across services |

### Typing

Pressing Enter after `function ...`, `if ... then`, `for ... do`, `while ... do`, `do` or `repeat` adds the closing `end` or `until` when the block is still open, and adds a missing `then` or `do` (`lunaut.completion.autocompleteEnd`). Typing `{` inside a `"string"` or `'string'` turns it into `` `string {` `` (`lunaut.format.convertQuotes`); the extension turns on `editor.formatOnType` for Luau so this works out of the box.

### Formatting

*Format Document* and *Format Selection* run [StyLua](https://github.com/JohnnyMorganz/StyLua) with the project's `stylua.toml`. Install StyLua or point `lunaut.format.styluaPath` at it.

### Navigation

| Feature | What it does |
| --- | --- |
| document highlight | marks every use of the symbol under the cursor in the file, writes and reads apart |
| document links | the path in `require("./util")` or `require("@alias/x")` is clickable |
| workspace symbols | `Ctrl+T` finds functions, locals and module members in every file |
| call hierarchy | *Show Call Hierarchy* lists callers and callees of a function, across modules |
| type names | a type reference jumps to its declaration; the name in `type Name = ...` jumps to the type body, so the internals are one click away |

### Type statistics

`lunaut stats` and the `lunaut/typeStats` request report how many types the workspace holds and where they come from: definition files (`@base`, `@roblox`), modules, and globals. The report also groups types by kind, lists exported aliases and their kinds, and shows the hierarchy of extern types (`Car extends Vehicle`). Type origins include internal and interface type counts for every module, and plugins can report their own analyzer status in the same payload.

```bash
lunaut stats src
lunaut stats src --json
```

### Colors, folding, highlighting

`Color3.new`, `fromRGB`, `fromHSV` and `fromHex` with literal arguments get a color square and a picker; the picked color is written back in the same constructor. Functions, blocks, tables, comment runs and `--#region` / `--#endregion` fold. Semantic highlighting marks parameters, locals, functions, methods, properties, enum items, types and built-in globals.

---

## References and rename

`Shift+F12` lists every use of the symbol under the cursor, `F2` renames it everywhere at once.

| Symbol | Where it is found |
| --- | --- |
| a local, a parameter, a local function | the file it lives in |
| a module member: `function Util.double()`, `Util.double`, `{ double = ... }` | the module and every file that requires it, open or not |

```luau
-- Util.luau
local Util = {}
function Util.double(value: number): number
	return value * 2
end
return Util

-- Main.server.luau
local Util = require(script.Parent.Util)
print(Util.double(21))
```

Renaming `double` in either file changes both. Members are matched by the table they belong to, not by name, so another table with a `double` field stays untouched. lunaut parses the whole workspace on the first request to know who requires whom; Rojo requires through `script`, `game` and `@game` count too. Names are checked before renaming: a keyword or a name that is not a valid identifier is refused. Globals from definition files (`print`, `game`) cannot be renamed.

---

## Requires and .luaurc

String requires follow the Luau rules:

| Require | Resolves to |
| --- | --- |
| `require("./util")` | `util.luau`, `util.lua`, `util/init.luau` next to the file |
| `require("../shared/util")` | one folder up |
| `require("@self/child")` | inside the folder of an `init.luau` |
| `require("@lib/math")` | the `lib` alias from the nearest `.luaurc` |

`.luaurc` and `.config.luau` files are read from the file's folder up to the root and merged, closer files win; in one folder `.luaurc` is applied after `.config.luau`. `languageMode`, `lint`, `lintErrors`, `typeErrors`, `globals` and `aliases` all work. Aliases are case-insensitive.

`.config.luau` is Luau code that returns the config, run in a sandbox with a 2 second limit:

```luau
return {
	luau = {
		languagemode = "strict",
		aliases = { tools = "./tools" },
	},
}
```

`lunaut.baseLuaurc` points to one more `.luaurc` that sits under all of them, handy for aliases shared by every project on your machine.

Inside `require("...")` completion lists the aliases and `./`, `../` first, then the folders and modules of whatever the typed path points to. `init.luau` files are shown as their folder.

---

## Command reference

| Command | What it does |
| --- | --- |
| `lunaut` / `lunaut lsp` | language server on stdio |
| `lunaut analyze [paths]` | checks files and folders (default `.`), prints diagnostics, exit code `1` on errors |
| `lunaut stats [paths] [--json]` | reports type statistics for files and folders (default `.`): totals, origins, kinds, hierarchy and aliases |
| `lunaut flags` | lists every Luau FFlag with its value and source |
| `lunaut version` | version, Luau version and compiler |
| `lunaut help` | usage |

Options for every command:

| Option | Meaning |
| --- | --- |
| `--flag:Name=Value` | set a Luau FFlag |
| `--no-flags-enabled` | do not enable `Luau*` flags by default |
| `--definition=[@name=]path[#priority]` | load a definition file, can repeat |
| `--settings=file` | extra settings in `.vscode/settings.json` format, applied last |
| `--verbose` | print the server log to stderr |
| `--json` | print `flags` or `stats` as JSON |

---

## Custom LSP methods

Besides the standard protocol the server answers:

| Method | Params | Result |
| --- | --- | --- |
| `lunaut/status` | `{}` | version, Luau version, solver, workspaces with platform, sourcemap and definitions |
| `lunaut/typeStats` | `{}` | type totals, origins, kinds, hierarchy and aliases for every workspace |
| `lunaut/fflags` | `{ "filter"?: string }` | `[{ name, value, kind, source }]` |
| `lunaut/definitions` | `{}` | `[{ root, definitions: [{ name, path, priority, success, message, source }] }]` |
| `lunaut/sourcemap` | `{ "regenerate"?: bool }` | regenerates and reloads the sourcemap of every Roblox workspace |

Other editors can pass settings in `initializationOptions` and `workspace/didChangeConfiguration` as `{ "lunaut": { "fflags": { "sync": false }, ... } }` (the keys of `lunaut.*` in snake_case), or just keep a `.vscode/settings.json` in the project.

---

## Environment variables

| Variable | Used by | Meaning |
| --- | --- | --- |
| `LUNAUT_VERSION` | installer | version to install, default `latest` |
| `LUNAUT_INSTALL_DIR` | installer, uninstaller | install folder |
| `LUNAUT_NO_PATH` | installer | do not touch `PATH` |
| `LUNAUT_NO_EXTENSION` | installer | do not install the VS Code extension |
| `LUNAUT_PURGE` | uninstaller | also remove the cache |
| `LUNAUT_REPOSITORY` | installer, uninstaller | GitHub repository to download from |
| `LUNAUT_BINARY` | tests, extension packaging | path to the server executable |
| `LUNAUT_OFFLINE` | server | never download anything, use the cache only (tests set it) |

---

## Tests

```bash
python tests/run.py
```

It runs every suite in [`tests/cases`](tests/cases) against `build/lunaut(.exe)` and writes [`tests/report.md`](tests/report.md) with every test, its status and time. Run one suite with `python tests/run.py test_rojo.py`, another binary with `--binary path`.

| Suite | Checks |
| --- | --- |
| `test_cli.py` | `version`, `help`, `analyze` output and exit codes |
| `test_lsp.py` | diagnostics, updates, hover, completion, definitions, symbols, signature help, aliases |
| `test_fflags.py` | defaults, `--no-flags-enabled`, `--flag`, bad values, overrides over LSP |
| `test_definitions.py` | priority order, `--!priority`, globs, a wrong order failing |
| `test_rojo.py` | sourcemap loading, instance requires, `script` and service types, completion of sourcemap children |
| `test_references.py` | references and rename for locals, module functions and table keys, in both solvers |
| `test_features.py` | inlay hints and their switches, documentation, signature help, auto-imports, semantic tokens, quick fixes, colors, folding, luau-lsp settings |
| `test_config.py` | `.config.luau` aliases, `lunaut.baseLuaurc`, `lunaut.analyzeLuaFiles`, `require` path completion, `const` hover |

On GitHub the [CI](.github/workflows/ci.yml) workflow builds and tests on Windows and Linux on every push and pull request. macOS runners are often unavailable for private repositories, so macOS has its own [macOS](.github/workflows/macos.yml) workflow: Actions → macOS → Run workflow. Releases try to build macOS too and ship without it if no runner is available.

Fixtures live in [`tests/fixtures`](tests/fixtures): a plain Luau project, two definition setups and a Rojo project with its own sourcemap and Roblox-like definitions, so tests need neither Rojo nor internet.

---

## Luau updates

The Luau version is a single line in [`luau.version`](luau.version). CMake fetches exactly that tag.

Every day [`update-luau.yml`](.github/workflows/update-luau.yml) runs [`scripts/update_luau.py`](scripts/update_luau.py): it finds the newest Luau release, writes it to `luau.version`, adds `Sync to upstream Luau X` to the changelog, builds lunaut, runs every test and opens a pull request. Merge it and the next release ships the new Luau. Run the same thing by hand:

```bash
python scripts/update_luau.py
```

---

## Releasing

Releases publish themselves. The version lives in one place, `project(lunaut VERSION x.y.z)` in `CMakeLists.txt`.

```bash
python scripts/release.py bump
```

Versions count up by `0.0.1` and carry over every ten, like an odometer: `0.0.1`, `0.0.2` … `0.0.9`, then `0.1.0`; after `0.9.9` comes `1.0.0`. `bump` without arguments picks the next one; it also takes an exact version like `0.3.0`. It writes the version into `CMakeLists.txt` and the VS Code extension and turns `## [Unreleased]` in `CHANGELOG.md` into `## [0.0.4] - <date>`. Commit and push to `main`.

On every push to `main` the [Release](.github/workflows/release.yml) workflow checks whether the tag `v<version>` exists. If it does not, it:

1. builds Windows, Linux and macOS (macOS is skipped when GitHub has no runner),
2. runs every test on each of them,
3. packages `lunaut.vsix` with the same version,
4. creates the tag `v<version>` and publishes the release with the changelog section as notes and every binary attached.

Pushes that do not change the version release nothing. To rebuild any commit under a chosen version, run the workflow by hand: Actions → Release → Run workflow, with `ref` (commit, branch or tag), `version` and `latest` (whether it becomes the latest release). The daily Luau update also bumps the patch version, so merging its pull request ships the new Luau on its own. `python scripts/release.py notes 0.2.0` prints what the release notes will be.

### Branch and tag rules

The rules live in [`.github/rulesets`](.github/rulesets):

| Ruleset | Protects | Rules |
| --- | --- | --- |
| `main` | the default branch | no deletion, no force push, changes through pull requests, `build (windows-latest)` and `build (ubuntu-latest)` must pass |
| `release tags` | tags `v*` | only the Release workflow and admins create them, nobody moves or deletes them |

Admins can bypass both, so you can still push to `main` directly. Apply or update them with

```bash
python scripts/apply_rulesets.py
```

or import the JSON files in Settings → Rules → Rulesets → Import a ruleset. GitHub offers rulesets for public repositories and on GitHub Pro and above; on a free private repository the script stops with GitHub's "Upgrade to GitHub Pro or make this repository public" message.

---

## Building from source

Needs CMake 3.20+, a C++17 compiler (MSVC, GCC or Clang, MinGW works) and git.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build
```

The result is `build/lunaut(.exe)`, statically linked. Luau and nlohmann/json are fetched by CMake; point to local copies with `-DFETCHCONTENT_SOURCE_DIR_LUAU=path` and `-DFETCHCONTENT_SOURCE_DIR_JSON=path`. `-DLUNAUT_DEBUG=ON` turns on debug logging.

Code style is in [`AGENTS.md`](AGENTS.md); format with `clang-format -i` and the root `.clang-format`.

---

## Source layout

```
src/Lunaut/
├── core.d.hpp        every public type: the contract
├── project.hpp       project definitions
├── build.hpp.in      version info, generated by CMake
├── fflag_core.hpp    core compile-time flags
├── fflag.hpp         feature flags
├── Program.cpp       entry point, arguments
├── Cli/              analyze mode
├── Server/           JSON-RPC, LSP dispatch, protocol replies
├── Core/             workspace, file resolver, FFlags, definitions, text documents
├── Features/         diagnostics, hover, completion, definition, references, rename, symbols, signature help
├── Models/           settings and enum names
├── Roblox/           sourcemap, instance requires, DataModel types, Rojo, Roblox types
└── Util/             strings, glob, uri, processes, http, cache
editors/vscode/       VS Code extension
installers/           install and uninstall scripts
scripts/              Luau update, release and ruleset scripts
tests/                test runner, suites and fixtures
```

---

## Limitations

- Definition file and FFlag changes need a server restart
- Rename and references cover locals and table members; globals and type names are not renamed
- Go to definition works for locals, requires and table members, not for globals from definition files
- Outside VS Code Rojo autogeneration runs `rojo sourcemap` once per change, it does not keep `--watch` running, and the Studio plugin is not available
- Downloads (FFlags, Roblox definitions) use `curl`, which ships with Windows 10+, macOS and most Linux systems

---

## Acknowledgements

lunaut is built on [Luau](https://github.com/luau-lang/luau). Many ideas, the Roblox type extensions and the Roblox API definitions come from [luau-lsp](https://github.com/JohnnyMorganz/luau-lsp) by JohnnyMorganz (MIT).

---

## License

MIT, see [LICENSE](LICENSE).
