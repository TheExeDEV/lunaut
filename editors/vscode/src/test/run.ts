import * as path from "path";
import { runTests } from "@vscode/test-electron";

async function main(): Promise<void> {
	const extensionDevelopmentPath = path.resolve(__dirname, "..", "..");
	const extensionTestsPath = path.resolve(__dirname, "suite");
	const workspace = path.resolve(extensionDevelopmentPath, "..", "..", "tests", "fixtures", "basic");

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
