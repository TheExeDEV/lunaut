import argparse
import datetime
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CMAKE = ROOT / "CMakeLists.txt"
PACKAGE = ROOT / "editors" / "vscode" / "package.json"
ZED_EXTENSION = ROOT / "editors" / "zed" / "extension.toml"
ZED_CARGO = ROOT / "editors" / "zed" / "Cargo.toml"
CHANGELOG = ROOT / "CHANGELOG.md"
VERSION_PATTERN = re.compile(r"^(\d+)\.(\d+)\.(\d+)(-[0-9A-Za-z.-]+)?$")
PROJECT_PATTERN = re.compile(r"(project\(lunaut VERSION )(\d+\.\d+\.\d+)")
MANIFEST_VERSION_PATTERN = re.compile(r'^(version = ")[^"]*(")', re.M)


def get_version():
	match = PROJECT_PATTERN.search(CMAKE.read_text(encoding="utf-8"))

	if not match:
		raise SystemExit("project version not found in CMakeLists.txt")

	return match.group(2)


def parse(version):
	match = VERSION_PATTERN.match(version)

	if not match:
		raise SystemExit(f"invalid version: {version}")

	return tuple(int(part) for part in match.groups()[:3])


def get_next(kind="next"):
	major, minor, patch = parse(get_version())

	if kind == "next":
		patch += 1

		if patch == 10:
			patch = 0
			minor += 1

		if minor == 10:
			minor = 0
			major += 1

		return f"{major}.{minor}.{patch}"

	if kind == "major":
		return f"{major + 1}.0.0"

	if kind == "minor":
		return f"{major}.{minor + 1}.0"

	return f"{major}.{minor}.{patch + 1}"


def write_versions(version):
	text = CMAKE.read_text(encoding="utf-8")
	CMAKE.write_text(PROJECT_PATTERN.sub(lambda match: match.group(1) + version.split("-")[0], text), encoding="utf-8")

	data = json.loads(PACKAGE.read_text(encoding="utf-8"))
	data["version"] = version
	PACKAGE.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

	for manifest in (ZED_EXTENSION, ZED_CARGO):
		text = manifest.read_text(encoding="utf-8")
		manifest.write_text(MANIFEST_VERSION_PATTERN.sub(lambda match: match.group(1) + version + match.group(2), text, count=1), encoding="utf-8")


def move_unreleased(version):
	lines = CHANGELOG.read_text(encoding="utf-8").splitlines()

	if "## [Unreleased]" not in lines:
		raise SystemExit("CHANGELOG.md has no [Unreleased] section")

	if f"## [{version}]" in " ".join(lines):
		raise SystemExit(f"CHANGELOG.md already has {version}")

	index = lines.index("## [Unreleased]")
	date = datetime.date.today().isoformat()
	lines[index:index + 1] = ["## [Unreleased]", "", f"## [{version}] - {date}"]
	CHANGELOG.write_text("\n".join(lines).rstrip("\n") + "\n", encoding="utf-8")


def get_notes(version):
	lines = CHANGELOG.read_text(encoding="utf-8").splitlines()

	for header in (f"## [{version}]", "## [Unreleased]"):
		start = next((i for i, line in enumerate(lines) if line.startswith(header)), None)

		if start is None:
			continue

		end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("## ")), len(lines))
		body = "\n".join(lines[start + 1:end]).strip()

		if body:
			return body

	return f"lunaut {version}"


def command_version(arguments):
	print(get_version())


def command_bump(arguments):
	target = arguments.version if arguments.version not in ("next", "major", "minor", "patch") else get_next(arguments.version)

	if parse(target) <= parse(get_version()) and "-" not in target:
		raise SystemExit(f"{target} is not newer than {get_version()}")

	write_versions(target)
	move_unreleased(target)
	print(target)


def command_notes(arguments):
	print(get_notes(arguments.version or get_version()))


def main():
	parser = argparse.ArgumentParser(description="lunaut release helper")
	commands = parser.add_subparsers(dest="command", required=True)

	commands.add_parser("version", help="print the current version").set_defaults(handler=command_version)

	bump = commands.add_parser("bump", help="set a new version: next (default, 0.0.9 -> 0.1.0), X.Y.Z, major, minor or patch")
	bump.add_argument("version", nargs="?", default="next")
	bump.set_defaults(handler=command_bump)

	notes = commands.add_parser("notes", help="print release notes from CHANGELOG.md")
	notes.add_argument("version", nargs="?")
	notes.set_defaults(handler=command_notes)

	arguments = parser.parse_args()
	arguments.handler(arguments)
	return 0


if __name__ == "__main__":
	sys.exit(main())
