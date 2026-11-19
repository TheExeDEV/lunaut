const childProcess = require("child_process");
const path = require("path");

const target = process.env.LUNAUT_TARGET || `${process.platform}-${process.arch}`;
const vsce = path.resolve(__dirname, "..", "node_modules", "@vscode", "vsce", "vsce");

childProcess.execFileSync(process.execPath, [vsce, "package", "--target", target, "--out", `lunaut-${target}.vsix`], {
	cwd: path.resolve(__dirname, ".."),
	stdio: "inherit",
});
