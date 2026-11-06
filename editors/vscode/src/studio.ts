import * as http from "http";
import * as vscode from "vscode";
import { LanguageClient } from "vscode-languageclient/node";

let server: http.Server | undefined;

const maximumBodySize = 16 * 1024 * 1024;

function getSetting<T>(key: string, fallback: T): T {
	const lunaut = vscode.workspace.getConfiguration("lunaut").inspect<T>(`studioPlugin.${key}`);
	const explicit = lunaut?.workspaceFolderValue ?? lunaut?.workspaceValue ?? lunaut?.globalValue;

	if (explicit !== undefined) {
		return explicit;
	}

	return vscode.workspace.getConfiguration("luau-lsp").get<T>(`studioPlugin.${key}`) ?? fallback;
}

function readBody(request: http.IncomingMessage): Promise<string> {
	return new Promise((resolve, reject) => {
		const chunks: Buffer[] = [];
		let size = 0;

		request.on("data", (chunk: Buffer) => {
			size += chunk.length;

			if (size > maximumBodySize) {
				reject(new Error("request body is too large"));
				request.destroy();
				return;
			}

			chunks.push(chunk);
		});
		request.on("end", () => resolve(Buffer.concat(chunks).toString("utf8")));
		request.on("error", reject);
	});
}

export function startStudioServer(getClient: () => LanguageClient | undefined, output: vscode.OutputChannel): void {
	stopStudioServer();

	if (!getSetting("enabled", false)) {
		return;
	}

	const port = getSetting("port", 3667);

	server = http.createServer(async (request, response) => {
		const client = getClient();

		try {
			if (request.method === "POST" && request.url === "/full") {
				const body = JSON.parse(await readBody(request));

				if (!client || !body.tree) {
					response.writeHead(client ? 400 : 500).end();
					return;
				}

				await client.sendNotification("$/plugin/full", body.tree);
				response.writeHead(200).end();
				return;
			}

			if (request.method === "POST" && request.url === "/clear") {
				if (!client) {
					response.writeHead(500).end();
					return;
				}

				await client.sendNotification("$/plugin/clear");
				response.writeHead(200).end();
				return;
			}

			if (request.method === "GET" && request.url === "/get-file-paths") {
				const uris = await vscode.workspace.findFiles("**/*.{lua,luau}");
				response.writeHead(200, { "Content-Type": "application/json" }).end(JSON.stringify({ files: uris.map((uri) => uri.fsPath) }));
				return;
			}

			response.writeHead(404).end();
		} catch (error) {
			response.writeHead(500).end(String(error));
		}
	});

	server.on("error", (error: NodeJS.ErrnoException) => {
		const reason = error.code === "EADDRINUSE" ? "the port is already in use (is luau-lsp running?)" : error.message;
		vscode.window.showErrorMessage(`Lunaut could not start the Studio plugin server on port ${port}: ${reason}`);
	});

	server.listen(port, "127.0.0.1", () => output.appendLine(`[studio] listening for the Studio plugin on port ${port}`));
}

export function stopStudioServer(): void {
	server?.close();
	server = undefined;
}
