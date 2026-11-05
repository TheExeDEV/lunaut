import json
import os
import pathlib
import queue
import subprocess
import threading
import time
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parent
FIXTURES = ROOT / "fixtures"


def find_binary():
	explicit = os.environ.get("LUNAUT_BINARY")

	if explicit:
		return pathlib.Path(explicit).resolve()

	for name in ("lunaut.exe", "lunaut"):
		candidate = ROOT.parent / "build" / name

		if candidate.exists():
			return candidate

	raise FileNotFoundError("lunaut binary not found, build it first or set LUNAUT_BINARY")


def to_uri(path):
	return pathlib.Path(path).resolve().as_uri()


def to_key(uri):
	path = urllib.parse.unquote(urllib.parse.urlparse(uri).path)

	if len(path) > 2 and path[0] == "/" and path[2] == ":":
		path = path[1:]

	return path.replace("\\", "/").lower()


def run_cli(*args, cwd=None, timeout=120):
	return subprocess.run(
		[str(find_binary()), *args],
		cwd=cwd,
		capture_output=True,
		text=True,
		encoding="utf-8",
		timeout=timeout,
	)


class LspError(Exception):
	pass


class LspClient:
	def __init__(self, root, options=None, args=()):
		self.root = pathlib.Path(root).resolve()
		self.process = subprocess.Popen(
			[str(find_binary()), "lsp", *args],
			cwd=self.root,
			stdin=subprocess.PIPE,
			stdout=subprocess.PIPE,
			stderr=subprocess.PIPE,
		)
		self.next_id = 1
		self.responses = {}
		self.notifications = queue.Queue()
		self.history = []
		self.lock = threading.Condition()
		self.reader = threading.Thread(target=self.read_loop, daemon=True)
		self.reader.start()
		self.stderr = threading.Thread(target=self.process.stderr.read, daemon=True)
		self.stderr.start()

		self.capabilities = self.request(
			"initialize",
			{
				"processId": os.getpid(),
				"rootUri": to_uri(self.root),
				"workspaceFolders": [{"uri": to_uri(self.root), "name": self.root.name}],
				"capabilities": {"workspace": {"didChangeWatchedFiles": {"dynamicRegistration": True}}},
				"initializationOptions": options or {},
			},
		)["capabilities"]
		self.notify("initialized", {})

	def read_loop(self):
		stream = self.process.stdout

		while True:
			length = 0

			while True:
				line = stream.readline()

				if not line:
					return

				line = line.decode("ascii").strip()

				if not line:
					break

				if line.lower().startswith("content-length:"):
					length = int(line.split(":", 1)[1])

			message = json.loads(stream.read(length).decode("utf-8"))

			if "id" in message and "method" in message:
				self.send({"jsonrpc": "2.0", "id": message["id"], "result": None})
				continue

			if "id" in message:
				with self.lock:
					self.responses[message["id"]] = message
					self.lock.notify_all()
				continue

			self.history.append(message)
			self.notifications.put(message)

	def send(self, message):
		body = json.dumps(message).encode("utf-8")
		self.process.stdin.write(b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body)
		self.process.stdin.flush()

	def request(self, method, params, timeout=120):
		identifier = self.next_id
		self.next_id += 1
		self.send({"jsonrpc": "2.0", "id": identifier, "method": method, "params": params})
		deadline = time.time() + timeout

		with self.lock:
			while identifier not in self.responses:
				remaining = deadline - time.time()

				if remaining <= 0:
					raise TimeoutError(f"no response to {method}")

				self.lock.wait(remaining)

			response = self.responses.pop(identifier)

		if "error" in response:
			raise LspError(response["error"])

		return response.get("result")

	def notify(self, method, params):
		self.send({"jsonrpc": "2.0", "method": method, "params": params})

	def wait_for(self, method, predicate=lambda params: True, timeout=120):
		deadline = time.time() + timeout

		while True:
			remaining = deadline - time.time()

			if remaining <= 0:
				raise TimeoutError(f"no {method} notification")

			message = self.notifications.get(timeout=remaining)

			if message.get("method") == method and predicate(message.get("params", {})):
				return message.get("params", {})

	def open(self, relative, text=None, timeout=120):
		path = self.root / relative

		if text is None:
			text = path.read_text(encoding="utf-8")

		uri = to_uri(path)
		self.notify("textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": "luau", "version": 1, "text": text}})
		params = self.wait_for("textDocument/publishDiagnostics", lambda params: to_key(params["uri"]) == to_key(uri), timeout)
		return uri, params["diagnostics"]

	def change(self, uri, text, version=2, timeout=120):
		self.notify(
			"textDocument/didChange",
			{"textDocument": {"uri": uri, "version": version}, "contentChanges": [{"text": text}]},
		)
		params = self.wait_for("textDocument/publishDiagnostics", lambda params: to_key(params["uri"]) == to_key(uri), timeout)
		return params["diagnostics"]

	def at(self, method, uri, line, character):
		return self.request(method, {"textDocument": {"uri": uri}, "position": {"line": line, "character": character}})

	def logs(self):
		return [message["params"]["message"] for message in self.history if message.get("method") == "window/logMessage"]

	def close(self):
		try:
			self.request("shutdown", None, timeout=30)
			self.notify("exit", None)
			self.process.wait(timeout=30)
		finally:
			if self.process.poll() is None:
				self.process.kill()

		return self.process.returncode


def position_of(text, needle, offset=0):
	index = text.index(needle) + offset
	line = text.count("\n", 0, index)
	character = index - (text.rfind("\n", 0, index) + 1)
	return line, character
