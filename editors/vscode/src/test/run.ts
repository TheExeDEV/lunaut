import * as fs from "fs";
import * as path from "path";
import { runTests } from "@vscode/test-electron";

function useFreshServer(extensionDevelopmentPath: string): void {
	const name = process.platform === "win32" ? "lunaut.exe" : "lunaut";
	const built = process.env.LUNAUT_BINARY || path.resolve(extensionDevelopmentPath, "..", "..", "build", name);

	if (!fs.existsSync(built)) {
		return;
	}

	const target = path.resolve(extensionDevelopmentPath, "bin", `${process.platform}-${process.arch}`, name);
	fs.mkdirSync(path.dirname(target), { recursive: true });
	fs.copyFileSync(built, target);
	fs.chmodSync(target, 0o755);
}

async function main(): Promise<void> {
	const extensionDevelopmentPath = path.resolve(__dirname, "..", "..");
	const extensionTestsPath = path.resolve(__dirname, "suite");
	const workspace = path.resolve(extensionDevelopmentPath, "..", "..", "tests", "fixtures", "basic");

	useFreshServer(extensionDevelopmentPath);

	await runTests({
		extensionDevelopmentPath,
		extensionTestsPath,
		launchArgs: [workspace, "--disable-extensions"],
	});
}

main().catch((error) => {
	console.error(error);
	process.exit(1);
});
