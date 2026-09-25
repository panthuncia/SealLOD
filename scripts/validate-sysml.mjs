#!/usr/bin/env node

import { spawn } from "node:child_process";
import { existsSync, readFileSync, readdirSync } from "node:fs";
import { homedir } from "node:os";
import { basename, dirname, isAbsolute, join, resolve } from "node:path";
import { pathToFileURL } from "node:url";

const sourceFiles = process.argv.slice(2).map((file) => resolve(file));
if (sourceFiles.length === 0) {
	console.error("usage: node validate-sysml.mjs <file.sysml> [file.kerml ...]");
	process.exit(2);
}

for (const file of sourceFiles) {
	if (!existsSync(file)) {
		console.error(`SysML source does not exist: ${file}`);
		process.exit(2);
	}
}

function findSpec42() {
	if (process.env.SPEC42_PATH) {
		const configured = isAbsolute(process.env.SPEC42_PATH)
			? process.env.SPEC42_PATH
			: resolve(process.env.SPEC42_PATH);
		if (existsSync(configured)) return configured;
		throw new Error(`SPEC42_PATH does not exist: ${configured}`);
	}

	const extensionRoot = join(homedir(), ".vscode", "extensions");
	if (!existsSync(extensionRoot)) return undefined;
	const candidates = readdirSync(extensionRoot)
		.filter((entry) => entry.startsWith("elan8.spec42-"))
		.sort()
		.reverse();
	for (const candidate of candidates) {
		const executable = join(extensionRoot, candidate, "server", "win32-x64", "spec42.exe");
		if (existsSync(executable)) return executable;
		const platform = process.platform === "darwin"
			? (process.arch === "arm64" ? "darwin-arm64" : "darwin-x64")
			: "linux-x64";
		const unixExecutable = join(extensionRoot, candidate, "server", platform, "spec42");
		if (existsSync(unixExecutable)) return unixExecutable;
	}
	return undefined;
}

const spec42 = findSpec42();
if (spec42) {
	function terminateProcessTree(child) {
		if (!child.pid) return;
		if (process.platform === "win32") {
			const killer = spawn("taskkill.exe", ["/PID", String(child.pid), "/T", "/F"], {
				stdio: "ignore",
				windowsHide: true,
			});
			killer.unref();
		} else {
			child.kill("SIGKILL");
		}
	}

	function runSpec42(file, timeoutMs) {
		return new Promise((resolveRun, rejectRun) => {
			const startedAt = Date.now();
			const child = spawn(spec42, [
				"check",
				file,
				"--workspace-root",
				process.cwd(),
				"--warnings-as-errors",
				"--format",
				"text",
			], {
				cwd: process.cwd(),
				stdio: "inherit",
				windowsHide: true,
			});
			let settled = false;
			const finish = (callback) => {
				if (settled) return;
				settled = true;
				clearTimeout(watchdog);
				callback();
			};
			const watchdog = setTimeout(() => {
				terminateProcessTree(child);
				finish(() => rejectRun(new Error(
					`Spec42 timed out after ${timeoutMs / 1000} seconds while validating ${file}. Child PID: ${child.pid ?? "unknown"}.`,
				)));
			}, timeoutMs);
			watchdog.unref();
			child.on("error", (error) => finish(() => rejectRun(error)));
			child.on("exit", (code, signal) => finish(() => resolveRun({
				code,
				signal,
				elapsedMs: Date.now() - startedAt,
			})));
		});
	}

	let failed = false;
	for (const file of sourceFiles) {
		const timeoutMs = 30_000;
		console.log(`Validating ${file} with Spec42 at ${spec42} (timeout: ${timeoutMs / 1000}s)...`);
		let result;
		try {
			result = await runSpec42(file, timeoutMs);
		} catch (error) {
			console.error(error instanceof Error ? error.message : String(error));
			process.exit(2);
		}
		if (result.signal) {
			console.error(`Spec42 was terminated by ${result.signal} while validating ${file}.`);
			process.exit(2);
		}
		console.log(`Spec42 finished ${file} in ${(result.elapsedMs / 1000).toFixed(2)}s.`);
		if (result.code !== 0) failed = true;
	}
	process.exit(failed ? 1 : 0);
}

function findSyside() {
	if (process.env.SYSIDE_PATH) {
		const configured = isAbsolute(process.env.SYSIDE_PATH)
			? process.env.SYSIDE_PATH
			: resolve(process.env.SYSIDE_PATH);
		if (existsSync(configured)) return configured;
		throw new Error(`SYSIDE_PATH does not exist: ${configured}`);
	}

	const extensionRoot = join(homedir(), ".vscode", "extensions");
	if (!existsSync(extensionRoot)) return undefined;
	const candidates = readdirSync(extensionRoot)
		.filter((entry) => entry.startsWith("sensmetry.syside-editor-") ||
			entry.startsWith("sensmetry.syside-modeler-"))
		.sort()
		.reverse();
	for (const candidate of candidates) {
		const executable = join(extensionRoot, candidate, "dist", "syside.exe");
		if (existsSync(executable)) return executable;
		const unixExecutable = join(extensionRoot, candidate, "dist", "syside");
		if (existsSync(unixExecutable)) return unixExecutable;
	}
	return undefined;
}

const syside = findSyside();
if (!syside) {
	console.error("Could not find the Syside executable. Set SYSIDE_PATH or install the Sensmetry Syside Editor extension.");
	process.exit(2);
}

const workspace = resolve(process.cwd());
const server = spawn(syside, [
	"server",
	"--stdio",
	"--edit",
	"all",
	"--log-level",
	"error",
	"--crash-reports",
	"ignore",
], {
	cwd: workspace,
	stdio: ["pipe", "pipe", "pipe"],
});

let receiveBuffer = Buffer.alloc(0);
let nextRequestID = 1;
let initialized = false;
let shuttingDown = false;
let idleTimer;
let hardTimer;
const diagnostics = new Map();
const expectedUris = new Set(sourceFiles.map((file) => pathToFileURL(file).href));
const pendingDiagnosticRequests = new Map();

function send(message) {
	const body = Buffer.from(JSON.stringify(message), "utf8");
	server.stdin.write(`Content-Length: ${body.length}\r\n\r\n`);
	server.stdin.write(body);
}

function respond(id, result = null) {
	send({ jsonrpc: "2.0", id, result });
}

function scheduleFinish() {
	if (![...expectedUris].every((uri) => diagnostics.has(uri))) return;
	clearTimeout(idleTimer);
	idleTimer = setTimeout(finish, 1200);
}

function printDiagnostics() {
	let errors = 0;
	let warnings = 0;
	for (const file of sourceFiles) {
		const uri = pathToFileURL(file).href;
		for (const diagnostic of diagnostics.get(uri) ?? []) {
			const severity = diagnostic.severity === 1 ? "error" :
				diagnostic.severity === 2 ? "warning" : "info";
			if (severity === "error") ++errors;
			if (severity === "warning") ++warnings;
			const line = (diagnostic.range?.start?.line ?? 0) + 1;
			const column = (diagnostic.range?.start?.character ?? 0) + 1;
			const rule = diagnostic.code === undefined ? "" : ` [${diagnostic.code}]`;
			console.error(`${file}:${line}:${column}: ${severity}${rule}: ${diagnostic.message}`);
		}
	}
	console.log(`Syside validated ${sourceFiles.length} file(s): ${errors} error(s), ${warnings} warning(s).`);
	return errors === 0 ? 0 : 1;
}

function finish() {
	if (shuttingDown) return;
	shuttingDown = true;
	clearTimeout(hardTimer);
	const exitCode = printDiagnostics();
	const id = nextRequestID++;
	send({ jsonrpc: "2.0", id, method: "shutdown", params: null });
	setTimeout(() => {
		send({ jsonrpc: "2.0", method: "exit", params: null });
		server.stdin.end();
		setTimeout(() => {
			server.kill();
			process.exit(exitCode);
		}, 250);
		process.exitCode = exitCode;
	}, 100);
}

function handleMessage(message) {
	if (message.method === "workspace/configuration" && message.id !== undefined) {
		respond(message.id, (message.params?.items ?? []).map(() => null));
		return;
	}
	if (message.method && message.id !== undefined) {
		respond(message.id);
		return;
	}
	if (message.method === "textDocument/publishDiagnostics") {
		diagnostics.set(message.params.uri, message.params.diagnostics ?? []);
		scheduleFinish();
		return;
	}
	if (pendingDiagnosticRequests.has(message.id)) {
		const uri = pendingDiagnosticRequests.get(message.id);
		pendingDiagnosticRequests.delete(message.id);
		if (message.error) {
			console.error(`Syside diagnostic request failed for ${uri}: ${message.error.message}`);
			process.exitCode = 2;
			server.kill();
			return;
		}
		diagnostics.set(uri, message.result?.items ?? []);
		scheduleFinish();
		return;
	}
	if (message.id === 1 && !initialized) {
		initialized = true;
		send({ jsonrpc: "2.0", method: "initialized", params: {} });
		for (const file of sourceFiles) {
			send({
				jsonrpc: "2.0",
				method: "textDocument/didOpen",
				params: {
					textDocument: {
						uri: pathToFileURL(file).href,
						languageId: file.endsWith(".kerml") ? "kerml" : "sysml",
						version: 1,
						text: readFileSync(file, "utf8"),
					},
				},
			});
		}
		setTimeout(() => {
			for (const uri of expectedUris) {
				const id = nextRequestID++;
				pendingDiagnosticRequests.set(id, uri);
				send({
					jsonrpc: "2.0",
					id,
					method: "textDocument/diagnostic",
					params: { textDocument: { uri } },
				});
			}
		}, 750);
	}
}

server.stdout.on("data", (chunk) => {
	receiveBuffer = Buffer.concat([receiveBuffer, chunk]);
	while (true) {
		const headerEnd = receiveBuffer.indexOf("\r\n\r\n");
		if (headerEnd < 0) break;
		const header = receiveBuffer.subarray(0, headerEnd).toString("ascii");
		const match = /Content-Length:\s*(\d+)/i.exec(header);
		if (!match) throw new Error(`Invalid LSP header: ${header}`);
		const bodyLength = Number.parseInt(match[1], 10);
		const bodyStart = headerEnd + 4;
		if (receiveBuffer.length < bodyStart + bodyLength) break;
		const body = receiveBuffer.subarray(bodyStart, bodyStart + bodyLength).toString("utf8");
		receiveBuffer = receiveBuffer.subarray(bodyStart + bodyLength);
		handleMessage(JSON.parse(body));
	}
});

server.stderr.on("data", (chunk) => process.stderr.write(chunk));
server.on("error", (error) => {
	console.error(`Failed to start Syside at ${syside}: ${error.message}`);
	process.exitCode = 2;
});
server.on("exit", (code) => {
	if (!shuttingDown && code !== 0) {
		console.error(`Syside exited before validation completed (exit code ${code}).`);
		process.exitCode = 2;
	}
});

send({
	jsonrpc: "2.0",
	id: nextRequestID++,
	method: "initialize",
	params: {
		processId: process.pid,
		clientInfo: { name: "BasicRenderer SysML validator", version: "1" },
		rootUri: pathToFileURL(workspace).href,
		workspaceFolders: [{ uri: pathToFileURL(workspace).href, name: basename(workspace) }],
		capabilities: {
			workspace: { configuration: true, workspaceFolders: true },
			textDocument: { publishDiagnostics: { relatedInformation: true } },
		},
	},
});

hardTimer = setTimeout(() => {
	console.error("Timed out waiting for Syside diagnostics.");
	server.kill();
	process.exit(2);
}, 30000);
