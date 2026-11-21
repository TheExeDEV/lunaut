const childProcess = require("child_process");
const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");
const host = `${process.platform}-${process.arch}`;
const binaries = path.resolve(root, "bin");
const manifest = path.resolve(root, "package.json");
const assets = {
	"win32-x64": "lunaut-win-x64.exe",
	"linux-x64": "lunaut-linux-x64",
	"darwin-x64": "lunaut-osx-x64",
	"darwin-arm64": "lunaut-osx-arm64",
};

function getName(platform) {
	return platform.startsWith("win32") ? "lunaut.exe" : "lunaut";
}

function bundle(platform, source) {
	const target = path.resolve(binaries, platform, getName(platform));
	fs.mkdirSync(path.dirname(target), { recursive: true });
	fs.copyFileSync(source, target);
	fs.chmodSync(target, 0o755);
	console.log(`bundled ${source} -> ${target}`);
	return target;
}

fs.rmSync(binaries, { recursive: true, force: true });

const bundled = {};

if (process.env.LUNAUT_BINARIES) {
	for (const [platform, asset] of Object.entries(assets)) {
		const source = path.resolve(process.env.LUNAUT_BINARIES, asset);

		if (fs.existsSync(source)) {
			bundled[platform] = bundle(platform, source);
		} else {
			console.warn(`no ${asset} in ${process.env.LUNAUT_BINARIES}, ${platform} users need lunaut on PATH`);
		}
	}
} else {
	const source = process.env.LUNAUT_BINARY || path.resolve(root, "..", "..", "build", getName(host));

	if (fs.existsSync(source)) {
		bundled[host] = bundle(host, source);
	}
}

if (Object.keys(bundled).length === 0) {
	console.error("no lunaut binary found, build the server first or set LUNAUT_BINARY or LUNAUT_BINARIES");
	process.exit(1);
}

fs.copyFileSync(path.resolve(root, "..", "..", "LICENSE"), path.resolve(root, "LICENSE"));

const studioPlugin = process.env.LUNAUT_STUDIO_PLUGIN || path.resolve(root, "..", "..", "build", "LunautPlugin.rbxm");

if (fs.existsSync(studioPlugin)) {
	fs.mkdirSync(path.resolve(root, "plugin"), { recursive: true });
	fs.copyFileSync(studioPlugin, path.resolve(root, "plugin", "LunautPlugin.rbxm"));
	console.log(`bundled the Studio plugin from ${studioPlugin}`);
} else {
	console.warn(`no Studio plugin at ${studioPlugin}, the extension will download it`);
}

const schemaBinary = process.env.LUNAUT_SCHEMA_BINARY || bundled[host];

if (!schemaBinary || !fs.existsSync(schemaBinary)) {
	console.warn("no lunaut that runs on this machine, keeping the current fflag schema");
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
