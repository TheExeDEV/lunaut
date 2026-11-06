import { ChildProcess, spawn } from "child_process";
import * as fs from "fs";
import * as path from "path";
import * as vscode from "vscode";

const watchers = new Map<string, ChildProcess>();

function getSetting<T>(key: string, luauLspKey: string, fallback: T): T {
	const lunaut = vscode.workspace.getConfiguration("lunaut").inspect<T>(key);
	const explicit = lunaut?.workspaceFolderValue ?? lunaut?.workspaceValue ?? lunaut?.globalValue;

	if (explicit !== undefined) {
		return explicit;
	}

	const luauLsp = vscode.workspace.getConfiguration("luau-lsp").get<T>(luauLspKey);
	return luauLsp ?? vscode.workspace.getConfiguration("lunaut").get<T>(key, fallback);
}

export function isWatching(): boolean {
	return watchers.size > 0;
}

export function startWatchers(output: vscode.OutputChannel): void {
	stopWatchers();

	const enabled = getSetting("rojo.enabled", "sourcemap.enabled", true);
	const autogenerate = getSetting("rojo.autogenerate", "sourcemap.autogenerate", true);
	const watch = vscode.workspace.getConfiguration("lunaut").get<boolean>("rojo.watch", true);

	if (!enabled || !autogenerate || !watch) {
		return;
	}

	const executable = getSetting("rojo.executable", "sourcemap.rojoPath", "rojo") || "rojo";
	const projectFile = getSetting("rojo.projectFile", "sourcemap.rojoProjectFile", "default.project.json");
	const sourcemapFile = getSetting("rojo.sourcemapFile", "sourcemap.sourcemapFile", "sourcemap.json");
	const includeNonScripts = getSetting("rojo.includeNonScripts", "sourcemap.includeNonScripts", true);

	for (const folder of vscode.workspace.workspaceFolders ?? []) {
		const root = folder.uri.fsPath;

		if (!fs.existsSync(path.join(root, projectFile))) {
			continue;
		}

		const args = ["sourcemap", projectFile, "--output", sourcemapFile, "--watch"];

		if (includeNonScripts) {
			args.push("--include-non-scripts");
		}

		const child = spawn(executable, args, { cwd: root, shell: process.platform === "win32" });
		watchers.set(root, child);
		output.appendLine(`[rojo] watching ${projectFile} in ${root}`);

		child.stderr?.on("data", (data) => output.appendLine(`[rojo] ${String(data).trim()}`));
		child.on("error", (error) => {
			output.appendLine(`[rojo] could not start ${executable}: ${error.message}`);
			watchers.delete(root);
		});
		child.on("exit", (code) => {
			if (watchers.get(root) === child) {
				watchers.delete(root);
				output.appendLine(`[rojo] sourcemap watcher stopped (exit code ${code}), the server will regenerate the sourcemap instead`);
			}
		});
	}
}

export function stopWatchers(): void {
	for (const child of watchers.values()) {
		child.kill();
	}

	watchers.clear();
}
