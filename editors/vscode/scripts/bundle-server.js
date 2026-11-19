const childProcess = require("child_process");
const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");
const host = `${process.platform}-${process.arch}`;
const target = process.env.LUNAUT_TARGET || host;
const name = target.startsWith("win32") ? "lunaut.exe" : "lunaut";
const hostName = process.platform === "win32" ? "lunaut.exe" : "lunaut";
const source = process.env.LUNAUT_BINARY || path.resolve(root, "..", "..", "build", name);
const binaries = path.resolve(root, "bin");
const bundled = path.resolve(binaries, name);
const manifest = path.resolve(root, "package.json");

if (!fs.existsSync(source)) {
	console.error(`lunaut binary not found at ${source}, build the server first or set LUNAUT_BINARY`);
	process.exit(1);
}

fs.rmSync(binaries, { recursive: true, force: true });
fs.mkdirSync(binaries, { recursive: true });
fs.copyFileSync(source, bundled);
fs.chmodSync(bundled, 0o755);
fs.copyFileSync(path.resolve(root, "..", "..", "LICENSE"), path.resolve(root, "LICENSE"));
console.log(`bundled ${source} -> ${bundled} for ${target}`);

const schemaBinary = process.env.LUNAUT_SCHEMA_BINARY || (target === host ? bundled : path.resolve(root, "..", "..", "build", hostName));

if (!fs.existsSync(schemaBinary)) {
	console.warn(`no runnable lunaut at ${schemaBinary}, keeping the current fflag schema`);
	process.exit(0);
}

const flags = JSON.parse(childProcess.execFileSync(schemaBinary, ["flags", "--json"], { encoding: "utf8" }));
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
