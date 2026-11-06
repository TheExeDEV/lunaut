import * as fs from "fs";
import * as os from "os";
import * as path from "path";
import * as vscode from "vscode";
import { LanguageClient, LanguageClientOptions, ServerOptions, TransportKind } from "vscode-languageclient/node";
import { isWatching, startWatchers, stopWatchers } from "./rojo";
import { startStudioServer, stopStudioServer } from "./studio";

let client: LanguageClient | undefined;
let output: vscode.OutputChannel | undefined;

const executableName = process.platform === "win32" ? "lunaut.exe" : "lunaut";

function expandHome(value: string): string {
	if (value === "~" || value.startsWith("~/") || value.startsWith("~\\")) {
		return path.join(os.homedir(), value.slice(1));
	}

	return value;
}

function getServerPath(context: vscode.ExtensionContext): string {
	const configured = expandHome(vscode.workspace.getConfiguration("lunaut").get<string>("path", ""));

	if (configured.length > 0) {
		return configured;
	}

	const bundled = context.asAbsolutePath(path.join("bin", executableName));

	if (fs.existsSync(bundled)) {
		return bundled;
	}

	return executableName;
}

function collectExplicit(section: string, flat: Record<string, unknown>): void {
	const config = vscode.workspace.getConfiguration(section);
	const visit = (value: unknown, prefix: string): void => {
		if (prefix.length > 0) {
			const inspected = config.inspect(prefix);
			const explicit = inspected?.workspaceFolderValue ?? inspected?.workspaceValue ?? inspected?.globalValue;

			if (explicit !== undefined) {
				flat[`${section}.${prefix}`] = explicit;
				return;
			}
		}

		if (value === null || typeof value !== "object" || Array.isArray(value)) {
			return;
		}

		for (const [key, child] of Object.entries(value as Record<string, unknown>)) {
			if (typeof child !== "function") {
				visit(child, prefix.length > 0 ? `${prefix}.${key}` : key);
			}
		}
	};

	visit(JSON.parse(JSON.stringify(config)), "");
}

function getServerSettings(): object {
	const flat: Record<string, unknown> = {};
	collectExplicit("luau-lsp", flat);
	collectExplicit("lunaut", flat);

	for (const key of ["lunaut.baseLuaurc", "luau-lsp.server.baseLuaurc"]) {
		if (typeof flat[key] === "string") {
			flat[key] = expandHome(flat[key] as string);
		}
	}

	if (isWatching()) {
		flat["lunaut.rojo.autogenerate"] = false;
	}

	return { vscode: flat };
}

async function startClient(context: vscode.ExtensionContext): Promise<void> {
	const command = getServerPath(context);

	const serverOptions: ServerOptions = {
		run: { command, args: ["lsp"], transport: TransportKind.stdio },
		debug: { command, args: ["lsp", "--verbose"], transport: TransportKind.stdio },
	};

	const clientOptions: LanguageClientOptions = {
		documentSelector: [
			{ scheme: "file", language: "luau" },
			{ scheme: "untitled", language: "luau" },
			{ scheme: "file", language: "lua" },
		],
		initializationOptions: getServerSettings(),
		outputChannel: output,
		synchronize: {
			fileEvents: [
				vscode.workspace.createFileSystemWatcher("**/*.{luau,lua}"),
				vscode.workspace.createFileSystemWatcher("**/.luaurc"),
				vscode.workspace.createFileSystemWatcher("**/sourcemap.json"),
				vscode.workspace.createFileSystemWatcher("**/*.project.json"),
			],
		},
	};

	client = new LanguageClient("lunaut", "Lunaut", serverOptions, clientOptions);
	await client.start();

	client.onNotification("lunaut/applyEdit", async (params: { edit: object; moveCursor: boolean }) => {
		const edit = await client!.protocol2CodeConverter.asWorkspaceEdit(params.edit as never);

		if (!edit) {
			return;
		}

		const applied = await vscode.workspace.applyEdit(edit);

		if (applied && params.moveCursor) {
			await vscode.commands.executeCommand("cursorMove", { to: "prevBlankLine" });
		}
	});
}

async function stopClient(): Promise<void> {
	if (client) {
		await client.stop();
		client = undefined;
	}
}

async function showJson(title: string, method: string, params: object = {}): Promise<void> {
	if (!client) {
		vscode.window.showWarningMessage("Lunaut is not running");
		return;
	}

	const result = await client.sendRequest(method, params);
	const document = await vscode.workspace.openTextDocument({ language: "json", content: JSON.stringify(result, null, 2) });
	await vscode.window.showTextDocument(document, { preview: true });
	output?.appendLine(`${title} opened`);
}

export async function activate(context: vscode.ExtensionContext): Promise<{ getClient: () => LanguageClient | undefined }> {
	output = vscode.window.createOutputChannel("Lunaut");
	context.subscriptions.push(output);

	context.subscriptions.push(
		vscode.commands.registerCommand("lunaut.restart", async () => {
			await stopClient();
			await startClient(context);
			vscode.window.showInformationMessage("Lunaut restarted");
		}),
		vscode.commands.registerCommand("lunaut.showStatus", () => showJson("Status", "lunaut/status")),
		vscode.commands.registerCommand("lunaut.showDefinitions", () => showJson("Definitions", "lunaut/definitions")),
		vscode.commands.registerCommand("lunaut.showFFlags", async () => {
			const filter = await vscode.window.showInputBox({ prompt: "Filter FFlags (empty shows all)" });

			if (filter !== undefined) {
				await showJson("FFlags", "lunaut/fflags", { filter });
			}
		}),
		vscode.commands.registerCommand("lunaut.regenerateSourcemap", () => showJson("Sourcemap", "lunaut/sourcemap", { regenerate: true })),
		vscode.workspace.onDidChangeConfiguration(async (event) => {
			if (!event.affectsConfiguration("lunaut") && !event.affectsConfiguration("luau-lsp")) {
				return;
			}

			if (event.affectsConfiguration("lunaut.rojo") || event.affectsConfiguration("luau-lsp.sourcemap")) {
				startWatchers(output!);
			}

			if (event.affectsConfiguration("lunaut.studioPlugin") || event.affectsConfiguration("luau-lsp.studioPlugin")) {
				startStudioServer(() => client, output!);
			}

			if (event.affectsConfiguration("lunaut.path") || event.affectsConfiguration("lunaut.fflags") || event.affectsConfiguration("luau-lsp.fflags")) {
				const choice = await vscode.window.showInformationMessage("Lunaut needs a restart to apply this change.", "Restart");

				if (choice === "Restart") {
					await vscode.commands.executeCommand("lunaut.restart");
				}

				return;
			}

			await client?.sendNotification("workspace/didChangeConfiguration", { settings: getServerSettings() });
		}),
	);

	startWatchers(output);
	startStudioServer(() => client, output);
	context.subscriptions.push({ dispose: () => stopWatchers() }, { dispose: () => stopStudioServer() });

	try {
		await startClient(context);
	} catch (error) {
		vscode.window.showErrorMessage(`Lunaut failed to start: ${error}. Set "lunaut.path" to the server executable.`);
	}

	return { getClient: () => client };
}

export async function deactivate(): Promise<void> {
	stopWatchers();
	stopStudioServer();
	await stopClient();
}
