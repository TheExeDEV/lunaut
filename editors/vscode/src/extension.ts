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

	const platforms = [`${process.platform}-${process.arch}`];

	if (process.platform === "darwin" && process.arch === "arm64") {
		platforms.push("darwin-x64");
	}

	for (const platform of platforms) {
		const bundled = context.asAbsolutePath(path.join("bin", platform, executableName));

		if (!fs.existsSync(bundled)) {
			continue;
		}

		if (process.platform !== "win32") {
			try {
				fs.accessSync(bundled, fs.constants.X_OK);
			} catch {
				fs.chmodSync(bundled, 0o755);
			}
		}

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

	return { vscode: flat, client: "lunaut-vscode" };
}

async function startClient(context: vscode.ExtensionContext): Promise<void> {
	const command = getServerPath(context);

	const serverOptions: ServerOptions = {
		run: { command, args: ["lsp"], transport: TransportKind.stdio },
		debug: { command, args: ["lsp", "--verbose"], transport: TransportKind.stdio },
	};

	const clientOptions: LanguageClientOptions = {
		middleware: {
			provideOnTypeFormattingEdits: async (document, position, character, options, token, next) => {
				const edits = await next(document, position, character, options, token);
				const editor = vscode.window.activeTextEditor;

				if (!edits || edits.length === 0 || !editor || editor.document !== document) {
					return edits;
				}

				let cursor = position;

				for (const edit of edits) {
					if (edit.range.end.isEqual(position) && edit.range.start.line === position.line && !edit.newText.includes("\n")) {
						cursor = new vscode.Position(position.line, edit.range.start.character + edit.newText.length);
					}
				}

				const applied = await editor.edit(
					(builder) => {
						for (const edit of edits) {
							builder.replace(edit.range, edit.newText);
						}
					},
					{ undoStopBefore: false, undoStopAfter: false },
				);

				if (!applied) {
					return edits;
				}

				editor.selection = new vscode.Selection(cursor, cursor);
				return [];
			},
		},
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

async function regenerateSourcemap(): Promise<void> {
	if (!client) {
		vscode.window.showWarningMessage("Lunaut is not running");
		return;
	}

	const results = await client.sendRequest<{ root: string; success: boolean; message: string; instances?: number }[]>("lunaut/sourcemap", {
		regenerate: true,
	});

	if (results.length === 0) {
		vscode.window.showWarningMessage("Lunaut: open a folder to regenerate its sourcemap");
		return;
	}

	for (const result of results) {
		const folder = path.basename(result.root) || result.root;

		if (result.success) {
			vscode.window.showInformationMessage(`Lunaut: sourcemap of ${folder} regenerated, ${result.instances ?? 0} instances`);
			continue;
		}

		vscode.window.showErrorMessage(`Lunaut: sourcemap of ${folder} was not regenerated: ${result.message}`, "Show Output").then((choice) => {
			if (choice === "Show Output") {
				output?.show();
			}
		});
	}
}

async function runServerCommand(command: string): Promise<void> {
	if (!client) {
		vscode.window.showWarningMessage("Lunaut is not running");
		return;
	}

	await client.sendRequest("workspace/executeCommand", { command, arguments: [] });
}

function getStudioPluginsFolder(): string | undefined {
	if (process.platform === "win32") {
		const local = process.env.LOCALAPPDATA;
		return local && fs.existsSync(path.join(local, "Roblox")) ? path.join(local, "Roblox", "Plugins") : undefined;
	}

	if (process.platform === "darwin") {
		const documents = path.join(os.homedir(), "Documents", "Roblox");
		return fs.existsSync(documents) ? path.join(documents, "Plugins") : undefined;
	}

	return undefined;
}

async function installStudioPlugin(context: vscode.ExtensionContext): Promise<void> {
	const bundled = context.asAbsolutePath(path.join("plugin", "LunautPlugin.rbxm"));

	if (!fs.existsSync(bundled)) {
		await runServerCommand("lunaut.server.installStudioPlugin");
		return;
	}

	const folder = getStudioPluginsFolder();

	if (!folder) {
		vscode.window.showErrorMessage("Lunaut: Roblox Studio was not found on this computer");
		return;
	}

	fs.mkdirSync(folder, { recursive: true });
	fs.copyFileSync(bundled, path.join(folder, "LunautPlugin.rbxm"));
	vscode.window.showInformationMessage(`Lunaut: installed the Roblox Studio plugin into ${folder}. Restart Studio and press Plugins > Lunaut.`);
}

type MenuEntry = vscode.QuickPickItem & { id?: string; run?: () => unknown; modal?: boolean; exit?: boolean; slow?: string };

async function updateSetting(key: string, value: unknown): Promise<void> {
	const config = vscode.workspace.getConfiguration("lunaut");
	const inspected = config.inspect(key);
	const target =
		inspected?.workspaceFolderValue !== undefined
			? vscode.ConfigurationTarget.WorkspaceFolder
			: inspected?.workspaceValue !== undefined
				? vscode.ConfigurationTarget.Workspace
				: vscode.ConfigurationTarget.Global;

	await config.update(key, value, target);

	const legacy = vscode.workspace.getConfiguration("luau-lsp").inspect(key);

	if (legacy?.workspaceFolderValue !== undefined || legacy?.workspaceValue !== undefined || legacy?.globalValue !== undefined) {
		vscode.window.showWarningMessage(`Lunaut: luau-lsp.${key} is also set; lunaut.${key} wins only where both are set at the same level`);
	}
}

async function changePort(): Promise<void> {
	const config = vscode.workspace.getConfiguration("lunaut");
	const current = config.get<number>("studioPlugin.port", 3667);
	const value = await vscode.window.showInputBox({
		title: "Lunaut: Studio plugin port",
		prompt: "The port the Studio plugin sends to. Use the same number in Plugins > Lunaut > Port.",
		value: String(current),
		validateInput: (text) => {
			const port = Number(text);
			return Number.isInteger(port) && port >= 1 && port <= 65535 ? undefined : "A whole number from 1 to 65535";
		},
	});

	if (value !== undefined && Number(value) !== current) {
		await updateSetting("studioPlugin.port", Number(value));
	}
}

function getMenuEntries(context: vscode.ExtensionContext): MenuEntry[] {
	const version = client?.initializeResult?.serverInfo?.version ?? context.extension.packageJSON.version;
	const config = vscode.workspace.getConfiguration("lunaut");
	const studio = config.get<boolean>("studioPlugin.enabled", false);
	const port = config.get<number>("studioPlugin.port", 3667);
	const repository = "https://github.com/TheExeDEV/lunaut";

	return [
		{
			id: "version",
			label: `$(zap) Lunaut v${version}`,
			detail: client ? "The server is running. Click for its status." : "The server is not running. Click to start it.",
			run: () => vscode.commands.executeCommand(client ? "lunaut.showStatus" : "lunaut.restart"),
		},
		{
			id: "install",
			label: "$(plug) Install Roblox Studio Plugin",
			description: "Click to install.",
			run: () => installStudioPlugin(context),
			slow: "installing the Roblox Studio plugin",
		},
		{ label: "Studio", kind: vscode.QuickPickItemKind.Separator },
		{
			id: "sync",
			label: studio ? "$(broadcast) Studio sync is on" : "$(circle-slash) Studio sync is off",
			description: studio ? `Listening on port ${port}. Click to stop.` : `Click to listen for the plugin on port ${port}.`,
			run: () => updateSetting("studioPlugin.enabled", !studio),
		},
		{ id: "port", label: "$(gear) Change Port", description: `Port ${port}`, run: () => changePort(), modal: true },
		{ label: "Server", kind: vscode.QuickPickItemKind.Separator },
		{ id: "restart", label: "$(debug-restart) Restart Server", run: () => vscode.commands.executeCommand("lunaut.restart"), slow: "restarting the server" },
		{ id: "reload", label: "$(refresh) Reload Definitions and FFlags", run: () => runServerCommand("lunaut.server.reloadWorkspace"), slow: "reloading definitions and FFlags, big projects take a few seconds" },
		{ id: "sourcemap", label: "$(sync) Regenerate Rojo Sourcemap", run: () => vscode.commands.executeCommand("lunaut.regenerateSourcemap"), slow: "regenerating the Rojo sourcemap" },
		{ id: "status", label: "$(info) Show Status", run: () => vscode.commands.executeCommand("lunaut.showStatus") },
		{ id: "fflags", label: "$(symbol-constant) Show FFlags", run: () => vscode.commands.executeCommand("lunaut.showFFlags"), modal: true },
		{ id: "definitions", label: "$(file-code) Show Loaded Definitions", run: () => vscode.commands.executeCommand("lunaut.showDefinitions") },
		{ label: "Help", kind: vscode.QuickPickItemKind.Separator },
		{ id: "docs", label: "$(book) Open Lunaut Docs", run: () => vscode.env.openExternal(vscode.Uri.parse(`${repository}#readme`)) },
		{ id: "issue", label: "$(github) Report an Issue", description: "Click to open a new issue on GitHub", run: () => vscode.env.openExternal(vscode.Uri.parse(`${repository}/issues/new`)) },
		{ label: "", kind: vscode.QuickPickItemKind.Separator },
		{ id: "exit", label: "$(close) Exit", description: "Or press Esc", exit: true },
	];
}

async function openMenu(context: vscode.ExtensionContext, activeId?: string): Promise<void> {
	const pick = vscode.window.createQuickPick<MenuEntry>();
	pick.title = "Lunaut";
	pick.ignoreFocusOut = true;
	pick.matchOnDescription = true;

	const refresh = (id?: string) => {
		pick.items = getMenuEntries(context);
		const active = pick.items.find((item) => item.id === id);

		if (active) {
			pick.activeItems = [active];
		}
	};

	pick.onDidAccept(async () => {
		const entry = pick.activeItems[0];

		if (!entry || entry.exit) {
			pick.hide();
			return;
		}

		if (entry.modal) {
			pick.hide();
			await entry.run?.();
			await openMenu(context, entry.id);
			return;
		}

		pick.busy = true;

		try {
			if (entry.slow) {
				await vscode.window.withProgress({ location: vscode.ProgressLocation.Notification, title: `Lunaut: ${entry.slow}` }, async () => {
					await entry.run?.();
				});
			} else {
				await entry.run?.();
			}
		} catch (error) {
			vscode.window.showErrorMessage(`Lunaut: ${error}`);
		} finally {
			pick.busy = false;
			refresh(entry.id);
		}
	});

	pick.onDidHide(() => pick.dispose());
	refresh(activeId);
	pick.show();
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

export async function activate(
	context: vscode.ExtensionContext,
): Promise<{ getClient: () => LanguageClient | undefined; getMenuEntries: () => MenuEntry[] }> {
	output = vscode.window.createOutputChannel("Lunaut");
	context.subscriptions.push(output);

	const statusItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 100);
	statusItem.text = "$(zap) Lunaut";
	statusItem.tooltip = "Open the Lunaut menu";
	statusItem.command = "lunaut.openMenu";
	statusItem.show();
	context.subscriptions.push(statusItem);

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
		vscode.commands.registerCommand("lunaut.regenerateSourcemap", () => regenerateSourcemap()),
		vscode.commands.registerCommand("lunaut.openMenu", () => openMenu(context)),
		vscode.commands.registerCommand("lunaut.installStudioPlugin", () => installStudioPlugin(context)),
		vscode.commands.registerCommand("lunaut.showReferences", (uri: string, position: { line: number; character: number }, locations: { uri: string; range: object }[]) => {
			const converter = client?.protocol2CodeConverter;

			if (!converter) {
				return;
			}

			return vscode.commands.executeCommand(
				"editor.action.showReferences",
				vscode.Uri.parse(uri),
				new vscode.Position(position.line, position.character),
				locations.map((location) => converter.asLocation(location as never)),
			);
		}),
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

			if (event.affectsConfiguration("lunaut.path")) {
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

	return { getClient: () => client, getMenuEntries: () => getMenuEntries(context) };
}

export async function deactivate(): Promise<void> {
	stopWatchers();
	stopStudioServer();
	await stopClient();
}
