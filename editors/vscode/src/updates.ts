import * as fs from "fs";
import * as path from "path";
import * as vscode from "vscode";

const latestReleaseUrl = "https://api.github.com/repos/TheExeDEV/lunaut/releases/latest";
const checkInterval = 6 * 60 * 60 * 1000;
const lastCheckKey = "lunaut.server.lastUpdateCheck";
const executableName = process.platform === "win32" ? "lunaut.exe" : "lunaut";

const assetNames: Record<string, string> = {
	"win32-x64": "lunaut-win-x64.exe",
	"linux-x64": "lunaut-linux-x64",
	"darwin-x64": "lunaut-osx-x64",
	"darwin-arm64": "lunaut-osx-arm64",
};

type Release = { tag_name?: string; assets?: { name: string; browser_download_url: string }[] };

export function compareVersions(left: string, right: string): number {
	const parse = (version: string) => version.replace(/^v/i, "").split(".").map((part) => parseInt(part, 10) || 0);
	const first = parse(left);
	const second = parse(right);

	for (let index = 0; index < Math.max(first.length, second.length); index++) {
		const difference = (first[index] ?? 0) - (second[index] ?? 0);

		if (difference !== 0) {
			return Math.sign(difference);
		}
	}

	return 0;
}

function getServersFolder(context: vscode.ExtensionContext): string {
	return path.join(context.globalStorageUri.fsPath, "server");
}

function getDownloadedVersions(context: vscode.ExtensionContext): string[] {
	const folder = getServersFolder(context);

	try {
		return fs
			.readdirSync(folder)
			.filter((version) => fs.existsSync(path.join(folder, version, executableName)))
			.sort((left, right) => compareVersions(right, left));
	} catch {
		return [];
	}
}

export function getDownloadedServer(context: vscode.ExtensionContext, bundledVersion: string): string | undefined {
	const newest = getDownloadedVersions(context)[0];

	if (!newest || compareVersions(newest, bundledVersion) <= 0) {
		return undefined;
	}

	return path.join(getServersFolder(context), newest, executableName);
}

function isAllowed(context: vscode.ExtensionContext): boolean {
	return (
		context.extensionMode !== vscode.ExtensionMode.Test &&
		process.env.LUNAUT_OFFLINE === undefined &&
		vscode.workspace.getConfiguration("lunaut").get<boolean>("server.autoUpdate", true) &&
		vscode.workspace.getConfiguration("lunaut").get<string>("path", "").length === 0
	);
}

async function download(url: string, target: string): Promise<void> {
	const response = await fetch(url, { headers: { "User-Agent": "lunaut-vscode" }, redirect: "follow" });

	if (!response.ok) {
		throw new Error(`${url} answered ${response.status}`);
	}

	const partial = `${target}.download`;
	fs.mkdirSync(path.dirname(target), { recursive: true });
	fs.writeFileSync(partial, Buffer.from(await response.arrayBuffer()));

	if (process.platform !== "win32") {
		fs.chmodSync(partial, 0o755);
	}

	fs.renameSync(partial, target);
}

function removeOlderVersions(context: vscode.ExtensionContext, keep: string): void {
	const folder = getServersFolder(context);

	for (const version of getDownloadedVersions(context)) {
		if (version === keep) {
			continue;
		}

		try {
			fs.rmSync(path.join(folder, version), { recursive: true, force: true });
		} catch {
			continue;
		}
	}
}

export async function checkForServerUpdate(context: vscode.ExtensionContext, runningVersion: string, output: vscode.OutputChannel, force = false): Promise<string | undefined> {
	if (!isAllowed(context)) {
		return undefined;
	}

	const lastCheck = context.globalState.get<number>(lastCheckKey, 0);

	if (!force && Date.now() - lastCheck < checkInterval) {
		return undefined;
	}

	const asset = assetNames[`${process.platform}-${process.arch}`];

	if (!asset) {
		return undefined;
	}

	await context.globalState.update(lastCheckKey, Date.now());

	const response = await fetch(latestReleaseUrl, { headers: { "User-Agent": "lunaut-vscode", Accept: "application/vnd.github+json" } });

	if (!response.ok) {
		throw new Error(`GitHub answered ${response.status}`);
	}

	const release = (await response.json()) as Release;
	const version = (release.tag_name ?? "").replace(/^v/i, "");
	const newest = [runningVersion, ...getDownloadedVersions(context)].sort((left, right) => compareVersions(right, left))[0];

	if (!version || compareVersions(version, newest) <= 0) {
		return undefined;
	}

	const file = release.assets?.find((item) => item.name === asset);

	if (!file) {
		return undefined;
	}

	output.appendLine(`[update] downloading lunaut ${version}`);
	await download(file.browser_download_url, path.join(getServersFolder(context), version, executableName));
	removeOlderVersions(context, version);
	output.appendLine(`[update] lunaut ${version} is ready, it starts with the next restart of the server`);
	return version;
}
