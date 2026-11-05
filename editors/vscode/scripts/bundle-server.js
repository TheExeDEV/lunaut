const childProcess = require("child_process");
const fs = require("fs");
const path = require("path");

const name = process.platform === "win32" ? "lunaut.exe" : "lunaut";
const root = path.resolve(__dirname, "..");
const source = process.env.LUNAUT_BINARY || path.resolve(root, "..", "..", "build", name);
const target = path.resolve(root, "bin", name);
const manifest = path.resolve(root, "package.json");

if (!fs.existsSync(source)) {
	console.error(`lunaut binary not found at ${source}, build the server first or set LUNAUT_BINARY`);
	process.exit(1);
}

fs.mkdirSync(path.dirname(target), { recursive: true });
fs.copyFileSync(source, target);
fs.chmodSync(target, 0o755);
fs.copyFileSync(path.resolve(root, "..", "..", "LICENSE"), path.resolve(root, "LICENSE"));
console.log(`bundled ${source} -> ${target}`);

const flags = JSON.parse(childProcess.execFileSync(target, ["flags", "--json"], { encoding: "utf8" }));
const properties = {};

for (const flag of flags) {
	properties[flag.name] =
		flag.kind === "bool"
			? { type: "string", enum: ["true", "false"], markdownDescription: `Boolean FFlag, default \`${flag.value}\`` }
			: { type: "string", pattern: "^-?\\d+$", markdownDescription: `Integer FFlag, default \`${flag.value}\`` };
}

const data = JSON.parse(fs.readFileSync(manifest, "utf8"));
const override = data.contributes.configuration.properties["lunaut.fflags.override"];
override.properties = properties;
override.additionalProperties = { type: "string" };
fs.writeFileSync(manifest, JSON.stringify(data, null, 2) + "\n");
console.log(`wrote ${flags.length} fflags into the lunaut.fflags.override schema`);
