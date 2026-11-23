# Security policy

## Supported versions

Only the latest release of lunaut gets security fixes. Update with `lunaut update`, the installer, or the VS Code extension, which also downloads new servers on its own.

| Version | Fixes |
| --- | --- |
| latest release | yes |
| older releases | no, update first |

## Reporting a vulnerability

Please do not open a public issue for a security problem. Report it privately through [GitHub's vulnerability reporting](https://github.com/TheExeDEV/lunaut/security/advisories/new) instead.

Include what an attacker can do, the lunaut version, the editor and system, and the smallest project or file that shows it. You get an answer within a week; once a fix is released, the advisory is published with credit unless you ask otherwise.

## What counts

lunaut reads the files of the project you open and runs a few things on your behalf. Problems in these places are security issues:

- **Opening a project** makes lunaut run something, write outside the project or the lunaut cache, or send data anywhere, beyond what is listed below
- **Settings** from a project's `.vscode/settings.json` run a program you did not choose, for example through `lunaut.rojo.executable`, `lunaut.format.styluaPath` or `lunaut.analyzer.path`
- **Downloads** (Roblox definitions, FFlags, the Studio plugin, `lunaut update`, the VS Code server updates) are taken from somewhere other than the documented addresses or are used without the checks described here
- **The Studio plugin connection** on `127.0.0.1` accepts data from anything other than the local Studio plugin, or lets a page in a browser change what the server does
- **Crashes or hangs** caused by a file, which a project could use to stop the editor from working

What lunaut does on purpose, so it is not a vulnerability by itself:

- it runs `rojo`, `stylua` and analyzer plugins that are named in the settings of the project you open, just like the editor runs the tasks of a workspace you trust
- it downloads Roblox API definitions and FFlags, and the Studio plugin and server updates, from GitHub and the Roblox client settings service
- it writes `sourcemap.json`, `.lunaut-declare` and its cache in `%LOCALAPPDATA%\lunaut` or `~/.cache/lunaut`

Open projects you do not trust in Restricted Mode, where VS Code does not start language servers.
