import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
VERSION_FILE = ROOT / "luau.version"
SUBMODULE = ROOT / "extern" / "luau"
CHANGELOG = ROOT / "CHANGELOG.md"
REPOSITORY = "https://github.com/luau-lang/luau"


def get_latest_tag():
	output = subprocess.check_output(["git", "ls-remote", "--tags", "--refs", REPOSITORY], text=True)
	versions = []

	for line in output.splitlines():
		match = re.search(r"refs/tags/(0\.(\d+))$", line)

		if match:
			versions.append((int(match.group(2)), match.group(1)))

	if not versions:
		raise RuntimeError("no Luau release tags found")

	return max(versions)[1]


def checkout_submodule(version):
	subprocess.check_call(["git", "-C", str(SUBMODULE), "fetch", "--depth", "1", "origin", f"refs/tags/{version}:refs/tags/{version}"], stdout=sys.stderr)
	subprocess.check_call(["git", "-C", str(SUBMODULE), "checkout", "--detach", version], stdout=sys.stderr)


def get_minor(version):
	return int(version.split(".")[1])


def update_changelog(version):
	message = f"- Sync to upstream Luau {version}"
	lines = CHANGELOG.read_text(encoding="utf-8").splitlines() if CHANGELOG.exists() else ["# Changelog"]

	if "## [Unreleased]" not in lines:
		index = 1 if lines and lines[0].startswith("# ") else 0
		lines[index:index] = ["", "## [Unreleased]", "", "### Changed", "", message]
	else:
		start = lines.index("## [Unreleased]")
		end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("## ")), len(lines))
		section = lines[start:end]
		existing = next((i for i, line in enumerate(section) if line.startswith("- Sync to upstream Luau ")), None)

		if existing is not None:
			lines[start + existing] = message
		elif "### Changed" in section:
			lines.insert(start + section.index("### Changed") + 2, message)
		else:
			lines[start + 1:start + 1] = ["", "### Changed", "", message]

	CHANGELOG.write_text("\n".join(lines).rstrip("\n") + "\n", encoding="utf-8")


def main():
	current = VERSION_FILE.read_text(encoding="utf-8").strip()
	latest = get_latest_tag()

	if get_minor(latest) <= get_minor(current):
		print(f"Luau {current} is up to date", file=sys.stderr)
		return 0

	checkout_submodule(latest)
	VERSION_FILE.write_text(latest + "\n", encoding="utf-8", newline="\n")
	update_changelog(latest)

	print(latest)
	return 0


if __name__ == "__main__":
	sys.exit(main())
