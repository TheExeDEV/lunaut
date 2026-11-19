import * as assert from "assert";
import * as path from "path";
import * as vscode from "vscode";

async function waitFor<T>(probe: () => T | undefined | Promise<T | undefined>, timeout = 60000): Promise<T> {
	const deadline = Date.now() + timeout;

	while (Date.now() < deadline) {
		const value = await probe();

		if (value !== undefined) {
			return value;
		}

		await new Promise((resolve) => setTimeout(resolve, 250));
	}

	throw new Error("timed out");
}

async function check(name: string, body: () => Promise<void>, results: string[]): Promise<void> {
	try {
		await body();
		results.push(`PASS ${name}`);
	} catch (error) {
		results.push(`FAIL ${name}: ${error}`);
	}
}

export async function run(): Promise<void> {
	const extension = vscode.extensions.getExtension("theexe.lunaut");
	assert.ok(extension, "extension is installed");

	const root = vscode.workspace.workspaceFolders![0].uri.fsPath;
	const main = vscode.Uri.file(path.join(root, "main.luau"));
	const results: string[] = [];

	await check(
		"activates on a luau file",
		async () => {
			const document = await vscode.workspace.openTextDocument(main);
			await vscode.window.showTextDocument(document);
			assert.strictEqual(document.languageId, "luau");
			await extension!.activate();
			assert.ok(extension!.isActive);
		},
		results,
	);

	await check(
		"publishes diagnostics",
		async () => {
			const diagnostics = await waitFor(() => {
				const list = vscode.languages.getDiagnostics(main);
				return list.length > 0 ? list : undefined;
			});
			assert.ok(diagnostics.some((diagnostic) => diagnostic.code === "TypeError"));
		},
		results,
	);

	await check(
		"provides hover",
		async () => {
			const hovers = await vscode.commands.executeCommand<vscode.Hover[]>("vscode.executeHoverProvider", main, new vscode.Position(2, 7));
			const text = hovers.map((hover) => hover.contents.map((part) => (part as vscode.MarkdownString).value).join("")).join("");
			assert.ok(text.includes("count: number"), text);
		},
		results,
	);

	await check(
		"provides completion",
		async () => {
			const list = await vscode.commands.executeCommand<vscode.CompletionList>(
				"vscode.executeCompletionItemProvider",
				main,
				new vscode.Position(4, 11),
			);
			const labels = list.items.map((item) => (typeof item.label === "string" ? item.label : item.label.label));
			assert.ok(labels.includes("greet"), labels.join(","));
		},
		results,
	);

	await check(
		"finds references",
		async () => {
			const locations = await vscode.commands.executeCommand<vscode.Location[]>("vscode.executeReferenceProvider", main, new vscode.Position(2, 7));
			assert.strictEqual(locations.length, 2, JSON.stringify(locations));
		},
		results,
	);

	await check(
		"renames across files",
		async () => {
			const edit = await vscode.commands.executeCommand<vscode.WorkspaceEdit>(
				"vscode.executeDocumentRenameProvider",
				main,
				new vscode.Position(4, 12),
				"welcome",
			);
			const files = edit.entries().map(([uri]) => path.basename(uri.fsPath)).sort();
			assert.deepStrictEqual(files, ["main.luau", "util.luau"]);
		},
		results,
	);

	await check(
		"goes to the type definition",
		async () => {
			const locations = await vscode.commands.executeCommand<(vscode.Location | vscode.LocationLink)[]>(
				"vscode.executeTypeDefinitionProvider",
				main,
				new vscode.Position(1, 7),
			);
			const files = locations.map((location) => ("uri" in location ? location.uri : location.targetUri).fsPath);
			assert.ok(
				files.some((file) => file.endsWith("util.luau")),
				JSON.stringify(files),
			);
		},
		results,
	);

	await check(
		"shows reference code lenses",
		async () => {
			const util = vscode.Uri.file(path.join(root, "util.luau"));
			await vscode.workspace.openTextDocument(util);
			const lenses = await vscode.commands.executeCommand<vscode.CodeLens[]>("vscode.executeCodeLensProvider", util, 10);
			const titles = lenses.map((lens) => lens.command?.title ?? "");
			assert.deepStrictEqual(titles, ["1 reference", "1 reference"]);
			assert.strictEqual(lenses[0].command?.command, "lunaut.showReferences");
		},
		results,
	);

	await check(
		"expands selection",
		async () => {
			const ranges = await vscode.commands.executeCommand<vscode.SelectionRange[]>("vscode.executeSelectionRangeProvider", main, [
				new vscode.Position(2, 31),
			]);
			assert.ok(ranges[0].parent?.parent, JSON.stringify(ranges));
		},
		results,
	);

	await check(
		"links required files",
		async () => {
			const links = await vscode.commands.executeCommand<vscode.DocumentLink[]>("vscode.executeLinkProvider", main);
			assert.ok(
				links.some((link) => link.target?.fsPath.endsWith("util.luau")),
				JSON.stringify(links),
			);
		},
		results,
	);

	await check(
		"finds workspace symbols",
		async () => {
			const symbols = await vscode.commands.executeCommand<vscode.SymbolInformation[]>("vscode.executeWorkspaceSymbolProvider", "gre");
			assert.ok(
				symbols.some((symbol) => symbol.name === "util.greet"),
				JSON.stringify(symbols.map((symbol) => symbol.name)),
			);
		},
		results,
	);

	const hintsFile = vscode.Uri.file(path.join(root, "hints.luau"));

	await check(
		"shows inlay hints",
		async () => {
			const document = await vscode.workspace.openTextDocument(hintsFile);
			await vscode.window.showTextDocument(document);
			const hints = await waitFor(async () => {
				const found = await vscode.commands.executeCommand<vscode.InlayHint[]>(
					"vscode.executeInlayHintProvider",
					hintsFile,
					new vscode.Range(0, 0, document.lineCount, 0),
				);
				return found.length > 0 ? found : undefined;
			});
			const labels = hints.map((hint) => (typeof hint.label === "string" ? hint.label : hint.label.map((part) => part.value).join("")));
			assert.ok(labels.includes(": string"), labels.join(","));
			assert.ok(labels.includes("value:"), labels.join(","));
		},
		results,
	);

	await check(
		"provides semantic tokens",
		async () => {
			const tokens = await vscode.commands.executeCommand<vscode.SemanticTokens>("vscode.provideDocumentSemanticTokens", hintsFile);
			assert.ok(tokens && tokens.data.length >= 5 && tokens.data.length % 5 === 0);
		},
		results,
	);

	await check(
		"provides folding ranges",
		async () => {
			const ranges = await vscode.commands.executeCommand<vscode.FoldingRange[]>("vscode.executeFoldingRangeProvider", hintsFile);
			assert.ok(ranges.some((range) => range.start === 1 && range.end === 2), JSON.stringify(ranges));
		},
		results,
	);

	await check(
		"provides colors",
		async () => {
			const document = await vscode.workspace.openTextDocument({ language: "luau", content: "local red = Color3.fromRGB(255, 0, 0)\n" });
			const colors = await vscode.commands.executeCommand<vscode.ColorInformation[]>("vscode.executeDocumentColorProvider", document.uri);
			assert.strictEqual(colors.length, 1);
			assert.strictEqual(colors[0].color.red, 1);
		},
		results,
	);

	await check(
		"passes changed settings to the server",
		async () => {
			await vscode.workspace.getConfiguration("lunaut").update("inlayHints.functionReturnTypes", false, vscode.ConfigurationTarget.Global);
			const api = extension!.exports as { getClient: () => { sendRequest: (method: string, params: object) => Promise<any> } | undefined };
			const settings = await waitFor(async () => {
				const status = await api.getClient()!.sendRequest("lunaut/status", {});
				const current = status.workspaces[0].settings;
				return current.inlay_hints.function_return_types === false ? current : undefined;
			});
			assert.strictEqual(settings.inlay_hints.function_return_types, false);
			await vscode.workspace.getConfiguration("lunaut").update("inlayHints.functionReturnTypes", undefined, vscode.ConfigurationTarget.Global);
		},
		results,
	);

	await check(
		"runs the status command",
		async () => {
			await vscode.commands.executeCommand("lunaut.showStatus");
			const editor = await waitFor(() => (vscode.window.activeTextEditor?.document.languageId === "json" ? vscode.window.activeTextEditor : undefined));
			assert.ok(editor.document.getText().includes("\"name\": \"lunaut\""));
		},
		results,
	);

	for (const line of results) {
		console.log(line);
	}

	const failed = results.filter((line) => line.startsWith("FAIL"));
	assert.strictEqual(failed.length, 0, failed.join("\n"));
}
