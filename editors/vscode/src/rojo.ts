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

const skippedFolders = new Set([".git", ".vscode", "node_modules", "Packages", "DevPackages", "ServerPackages", "build", "out", "target"]);

function collectProjects(folder: string, depth: number, projects: string[]): void {
	let entries: fs.Dirent[];

	try {
		entries = fs.readdirSync(folder, { withFileTypes: true });
	} catch {
		return;
	}

	for (const entry of entries) {
		const full = path.join(folder, entry.name);

		if (entry.isDirectory()) {
			if (depth < 4 && !skippedFolders.has(entry.name)) {
				collectProjects(full, depth + 1, projects);
			}
		} else if (entry.name.endsWith(".project.json")) {
			projects.push(full);
		}
	}
}

function findProject(root: string, projectFile: string): string | undefined {
	if (fs.existsSync(path.join(root, projectFile))) {
		return projectFile;
	}

	const preferred = path.basename(projectFile);
	const candidates: string[] = [];
	collectProjects(root, 0, candidates);

	const ranked = candidates
		.map((candidate) => path.relative(root, candidate).split(path.sep).join("/"))
		.sort((left, right) => {
			const depth = left.split("/").length - right.split("/").length;
			const named = Number(path.posix.basename(right) === preferred) - Number(path.posix.basename(left) === preferred);
			return depth || named || left.localeCompare(right);
		});

	return ranked[0];
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
		const project = findProject(root, projectFile);

		if (!project) {
			continue;
		}

		if (project.split(/[\\/]/).length > 1) {
			output.appendLine(`[rojo] ${project} is in a subfolder, so rojo --watch would write paths relative to it; the server regenerates the sourcemap instead`);
			continue;
		}

		const args = ["sourcemap", project, "--output", sourcemapFile, "--watch"];

		if (includeNonScripts) {
			args.push("--include-non-scripts");
		}

		const child = spawn(executable, args, { cwd: root, shell: process.platform === "win32" });
		watchers.set(root, child);
		output.appendLine(`[rojo] watching ${project} in ${root}`);

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
