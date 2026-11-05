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
	const extension = vscode.extensions.getExtension("lunaut.lunaut");
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
