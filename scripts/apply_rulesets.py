import argparse
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
RULESETS = ROOT / ".github" / "rulesets"


def call(arguments, payload=None):
	result = subprocess.run(
		["gh", "api", *arguments, *(["--input", "-"] if payload is not None else [])],
		input=json.dumps(payload) if payload is not None else None,
		capture_output=True,
		text=True,
	)

	if result.returncode != 0:
		raise SystemExit(result.stderr.strip() or result.stdout.strip())

	return json.loads(result.stdout) if result.stdout.strip() else None


def get_repository():
	return subprocess.check_output(["gh", "repo", "view", "--json", "nameWithOwner", "--jq", ".nameWithOwner"], text=True).strip()


def main():
	parser = argparse.ArgumentParser(description="Create or update the GitHub rulesets from .github/rulesets")
	parser.add_argument("--repository", help="owner/name, defaults to the current repository")
	arguments = parser.parse_args()

	repository = arguments.repository or get_repository()
	existing = {ruleset["name"]: ruleset["id"] for ruleset in call([f"repos/{repository}/rulesets"]) or []}

	for file in sorted(RULESETS.glob("*.json")):
		payload = json.loads(file.read_text(encoding="utf-8"))
		name = payload["name"]

		if name in existing:
			call(["-X", "PUT", f"repos/{repository}/rulesets/{existing[name]}"], payload)
			print(f"updated ruleset '{name}'")
		else:
			call(["-X", "POST", f"repos/{repository}/rulesets"], payload)
			print(f"created ruleset '{name}'")

	return 0


if __name__ == "__main__":
	sys.exit(main())
