# Contributing to lunaut

Thanks for helping! Bug reports, ideas and pull requests are all welcome. This page explains how to build lunaut, run the tests and get a change merged.

## Reporting a bug or asking for a feature

Open an [issue](https://github.com/TheExeDEV/lunaut/issues/new/choose) and pick a template. The smaller the example, the faster it gets fixed: a few lines of Luau, the `.d.luau` or settings that matter, the lunaut version (`lunaut version` or the first line of the Lunaut menu in VS Code) and the server log (Output > Lunaut in VS Code).

Security problems go through a private report instead, see [SECURITY.md](SECURITY.md).

## Building

You need CMake 3.20+, a C++17 compiler (MSVC, GCC or Clang; MinGW works), Ninja, Python 3 and git. Luau is a git submodule, so clone with it:

```bash
git clone --recursive https://github.com/TheExeDEV/lunaut.git
cd lunaut
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

In a clone made without `--recursive`, run `git submodule update --init` first. The server ends up in `build/lunaut` (`build/lunaut.exe` on Windows). Run one build at a time: two builds in the same folder can corrupt Ninja's log.

## Running the tests

```bash
python tests/run.py
```

This runs every suite in [`tests/cases`](tests/cases) against `build/lunaut` and writes `tests/report.md`. Run one suite with `python tests/run.py test_rojo.py`. Some Rojo tests need a real `rojo`; without it they are skipped.

The VS Code extension has its own tests, which start a real VS Code:

```bash
cd editors/vscode
npm ci
npm test
```

`npm test` copies `build/lunaut` into the extension first, so build the server before running it.

Every change in behavior needs a test. A good test fails without your change and passes with it.

## Code style

The rules are in [AGENTS.md](AGENTS.md). In short:

- **Public types live in [`src/Lunaut/core.d.hpp`](src/Lunaut/core.d.hpp)**; a source file implements what is declared there and adds no public members of its own
- **One folder per layer:** `Cli/`, `Server/`, `Core/`, `Features/` (one LSP operation per file), `Models/`, `Roblox/`, `Util/`
- **`snake_case` for everything**, methods start with a verb (`get_`, `set_`, `is_`, `apply_`...), files and folders are `PascalCase`
- **`result<T>`** for anything that can fail, and `relevant_*` tables instead of long `if`/`else` chains over strings or enums
- **No comments in code**: names explain it
- **Formatting** comes from `.clang-format` with clang-format **23.1.5** (`pip install clang-format==23.1.5`): tabs, Allman braces, braces always, explicit types instead of `auto`
- **Files** are UTF-8 with LF line endings

## Settings and compatibility

- New settings use the `lunaut.` prefix and camelCase names, and go into `editors/vscode/package.json` with a description
- Where luau-lsp has the same option, read its `luau-lsp.` key too
- New behavior that changes what people are used to is off by default
- Do not rename or remove keys of `lunaut.definitions`, `.lunaut-declare` or other `lunaut.*` settings: tools such as [crater](https://github.com/TheExeDEV/crater) rely on them. Only add new ones

## Pull requests

1. Fork the repository and create a branch from `main`
2. Make the change with its tests, and add a line to the `## [Unreleased]` section of [CHANGELOG.md](CHANGELOG.md) under `Added`, `Changed`, `Fixed` or `Removed`
3. Update the [README](README.md) when you add or change a setting, a command or user-visible behavior
4. Run `clang-format -i` on the C++ files you touched, `python tests/run.py`, and `npm test` if you changed the extension
5. Open the pull request and describe what it changes and why

Every pull request runs these checks, and all of them must pass:

| Check | What it looks at |
| --- | --- |
| CI | builds lunaut on Windows and Linux and runs every test |
| Project rules | clang-format, no comments added in C++, no version changes, LF line endings, test scripts compile |
| Installers | both halves of `installers/install` and `installers/uninstall` parse |
| VS Code extension | the extension compiles |
| Zed extension | the Zed extension compiles |
| Coverage | reports coverage to Codecov (for information only) |

Do not change the version in `CMakeLists.txt`, `package.json` or the Zed manifests: versions are raised when a release is made. New Luau versions are picked up automatically every day, so there is no need to update `extern/luau` by hand.

## Other parts of the repository

| Folder | What it is | How to build it |
| --- | --- | --- |
| [`editors/vscode`](editors/vscode) | the VS Code extension | `npm ci`, then `npm run package` makes `lunaut.vsix` |
| [`editors/zed`](editors/zed) | the Zed extension | install Rust with rustup, then in Zed: Extensions > Install Dev Extension, pick this folder |
| [`plugins/studio`](plugins/studio) | the Roblox Studio plugin | `rojo build plugins/studio -o LunautPlugin.rbxm` |
| [`installers`](installers) | install and uninstall scripts for every system | each file is valid as both `sh` and PowerShell |

## License

By contributing, you agree that your contribution is released under the [MIT license](LICENSE) of this project.
