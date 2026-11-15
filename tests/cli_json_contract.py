"""Test CLI JSON documents, exit statuses and real cancellation on an isolated TLS Server."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import ctypes
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import signal
import socket
import socketserver
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time

from cli_management import i64, sql_server, text
from cli_smoke import certificate, require, run as run_server

CORRELATION_TOKEN = re.compile("[0-9a-f]{32}")
SQL_ERROR = 5
AUTH_ERROR = 3
CANCELLED = 6
UNKNOWN = 7
ACTION_TIMEOUT = 17


MAX_FRAME = 1024 * 1024


def receive(socket_, length):
    data = bytearray()
    while len(data) < length:
        packet = socket_.recv(length - len(data))
        if not packet:
            raise EOFError
        data.extend(packet)
    return bytes(data)


def frame(socket_):
    header = receive(socket_, 4)
    size, = struct.unpack("!I", header)
    require(0 < size <= MAX_FRAME, "Invalid fixture frame length")
    payload = receive(socket_, size)
    return header + payload, json.loads(payload)


@contextmanager
def tls_client(port, context):
    with socket.create_connection(("127.0.0.1", port), timeout=12) as tcp:
        with context.wrap_socket(tcp, server_hostname="localhost", suppress_ragged_eofs=False) as connection:
            yield connection


class Wire:
    def __init__(self, socket_):
        self.socket = socket_
        self.id = 0

    def call(self, operation, **body):
        self.id += 1
        data = json.dumps({"v": 1, "id": self.id, "op": operation, "body": body},
                          separators=(",", ":")).encode("utf-8")
        self.socket.sendall(struct.pack("!I", len(data)) + data)
        _, reply = frame(self.socket)
        require(reply["id"] == self.id and reply["v"] == 1, "Incorrect independent Server response ID")
        require(reply["ok"] is True, f"Independent Server rejected {operation} with code "
                f"{reply.get('error', {}).get('code')}")
        return reply["body"]

    def query(self, sql):
        statement = self.call("query.prepare", sql=sql)["statement_id"]
        try:
            result_id = self.call("execute", statement_id=statement, parameters={})["result_id"]
            rows, columns = [], []
            for index in range(64):
                batch = self.call("fetch", result_id=result_id, batch_rows=64)["batch"]
                require((batch["result_set"], batch["batch_index"]) == (0, index),
                        "Independent Server verification returned out-of-sequence rows")
                if index == 0:
                    columns = batch["columns"]
                rows.extend(batch["rows"])
                if batch["end_of_results"]:
                    return rows, columns
            raise RuntimeError("Independent fixture SELECT exceeded its expected row limit")
        finally:
            self.call("statement_close", statement_id=statement)


@contextmanager
def client(port, context, password):
    with tls_client(port, context) as socket_:
        wire = Wire(socket_)
        wire.call("hello")
        wire.call("auth", username="local-admin", password=password)
        yield wire


def write(connection, sql, expected, *, types=None, values=None):
    prepared = connection.call("query.prepare", sql=sql, parameter_types=types or {})
    statement = prepared["statement_id"]
    try:
        reply = connection.call("execute", statement_id=statement, parameters=values or {},
                                request_token=secrets.token_hex(16))
        require(reply["affected_rows"] == expected, "Independent fixture seed was not written")
    finally:
        connection.call("statement_close", statement_id=statement)


class DropCommit(socketserver.ThreadingTCPServer):
    daemon_threads = True

    def __init__(self, port, context, fixture):
        super().__init__(("127.0.0.1", 0), ProxyConnection)
        self.port = port
        self.context = context
        self.local_tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.local_tls.minimum_version = ssl.TLSVersion.TLSv1_2
        self.local_tls.load_cert_chain(fixture / "server.pem", fixture / "server.key")
        self.receipt = None
        self.failure = False
        self.lock = threading.Lock()

    def drop(self, request, reply):
        require(request["v"] == reply["v"] == 1 and request["id"] == reply["id"], "Incorrect loopback relay ID")
        if request["op"] != "tx_commit" or not reply["ok"]:
            return False
        key, lsn = request["body"]["request_token"], reply["body"]["lsn"]
        require(CORRELATION_TOKEN.fullmatch(key) and lsn, "A committed response lacked its token or LSN")
        with self.lock:
            if self.receipt is None:
                self.receipt = key, lsn
                return True
        return False

    def handle_error(self, request, client_address):
        self.failure = True


class ProxyConnection(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(12)
        try:
            with self.server.local_tls.wrap_socket(self.request, server_side=True) as caller:
                with tls_client(self.server.port, self.server.context) as upstream:
                    while True:
                        data, request = frame(caller)
                        upstream.sendall(data)
                        data, reply = frame(upstream)
                        if self.server.drop(request, reply):
                            caller.shutdown(socket.SHUT_RDWR)
                            return
                        caller.sendall(data)
        except (EOFError, ssl.SSLEOFError, ConnectionResetError, BrokenPipeError):
            pass


@contextmanager
def relay(port, context, fixture):
    with DropCommit(port, context, fixture) as proxy:
        worker = threading.Thread(target=proxy.serve_forever, daemon=True)
        worker.start()
        try:
            yield proxy
        finally:
            proxy.shutdown()
            worker.join(timeout=3)


def clean_output(data, credentials):
    text_ = data.decode("utf-8", errors="replace")
    for credential in credentials:
        text_ = text_.replace(credential, "<redacted>")
    return text_[:20000]


def unique_keys(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON property {key!r}")
        result[key] = value
    return result


def invalid_number(_):
    raise ValueError("machine output contained a nonfinite JSON number")


def check(result, expected, credentials, title):
    streams = result.stdout + result.stderr
    require(all(value.encode("ascii") not in streams for value in credentials), f"{title} exposed credentials")
    require(b"\x1b" not in result.stdout, f"{title} stdout contained ANSI")
    output = clean_output(result.stdout, credentials)
    errors = clean_output(result.stderr, credentials)
    require(result.returncode == expected,
            f"{title} exited {result.returncode}, expected {expected}; stdout={output!r}, stderr={errors!r}")
    try:
        body = json.loads(result.stdout.decode("utf-8"), object_pairs_hook=unique_keys, parse_constant=invalid_number)
    except (UnicodeError, ValueError) as error:
        raise RuntimeError(f"{title} stdout was not one pure UTF-8 JSON document: {output!r}") from error
    require(type(body) is dict and type(body.get("exit_code")) is int and body["exit_code"] == result.returncode
            and type(body.get("events")) is list,
            f"{title} JSON disagreed with the process status: {output!r}")
    require(all(type(event) is dict and type(event.get("type")) is str and type(event.get("data")) is dict
                for event in body["events"]), f"{title} contained malformed output events: {output!r}")
    print(f"{title}: exit {expected}, strict UTF-8 JSON parsed", flush=True)
    return body["events"]


def errors(events):
    return [event["data"] for event in events if event["type"] == "error"]


def run(command, credential, cwd):
    try:
        return subprocess.run(command, cwd=cwd, input=(credential + "\n").encode("ascii"),
                              capture_output=True, timeout=ACTION_TIMEOUT, check=False)
    except subprocess.TimeoutExpired:
        raise RuntimeError("CLI batch exceeded its fixture deadline") from None


def cli_args(cli, fixture, port):
    return [str(cli), "--host", "127.0.0.1", "--port", str(port), "--tls-name", "localhost",
            "--ca", str(fixture / "server.pem"), "--user", "local-admin", "--password-stdin",
            "--format", "json", "--color", "always", "--timeout-ms", "9000"]


@contextmanager
def running_cli(command, cwd, password):
    control = ({"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == "nt"
               else {"start_new_session": True})
    with subprocess.Popen(command, cwd=cwd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, **control) as process:
        try:
            process.stdin.write((password + "\n").encode("ascii"))
            process.stdin.close()
            process.stdin = None
            yield process
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=6)


def send_interrupt(process):
    require(process.poll() is None, "CLI exited before its console event")
    if os.name == "nt":
        # The worker and its children have a private, hidden console. A targeted
        # CTRL_BREAK_EVENT can reach the CLI's group without reaching the worker.
        console = ctypes.WinDLL("kernel32", use_last_error=True)
        generate = console.GenerateConsoleCtrlEvent
        generate.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
        generate.restype = ctypes.c_int
        require(generate(signal.CTRL_BREAK_EVENT, process.pid),
                f"Could not send the targeted console event ({ctypes.get_last_error()})")
    else:
        os.killpg(process.pid, signal.SIGINT)


def wait_for_blocked_query(controller, previous, process):
    end = time.monotonic() + 6
    while time.monotonic() < end and process.poll() is None:
        sessions = controller.call("session_list")["sessions"]
        blocked = [session for session in sessions if session["session_id"] not in previous
                   and session["operation"] == "query.prepare" and session["lock_wait_ms"] > 0
                   and session["request_id"] > 0]
        if blocked:
            require(len(blocked) == 1, "more than one query matched the isolated cancellation probe")
            return blocked[0]
        time.sleep(0.025)
    raise RuntimeError("CLI did not show a waiting request in the real Server session list")


def test_interrupt(command, cwd, port, context, password, creds):
    with client(port, context, password) as holder, client(port, context, password) as controller:
        previous = {session["session_id"] for session in controller.call("session_list")["sessions"]}
        holder.call("tx_begin")
        try:
            with running_cli([*command, "--command", "SELECT id FROM cli_t02"], cwd, password) as process:
                blocked = wait_for_blocked_query(controller, previous, process)
                require(not blocked["transaction_active"], "cancellation selected the lock owner")
                send_interrupt(process)
                try:
                    out, err = process.communicate(timeout=7)
                except subprocess.TimeoutExpired:
                    raise RuntimeError("CLI did not finish after targeted cancellation") from None
                result = subprocess.CompletedProcess(process.args, process.returncode, out, err)
                failure = errors(check(result, CANCELLED, creds, "Active request console cancellation"))
                require(len(failure) == 1 and failure[0].get("code") == 1009
                        and failure[0].get("unknown_outcome") is False,
                        f"CLI did not report a definite correlated cancellation: {failure!r}")
            sessions = controller.call("session_list")["sessions"]
            require(any(s["session_id"] in previous and s["transaction_active"] for s in sessions),
                    "cancellation released the unrelated lock holder")
        finally:
            holder.call("tx_rollback")
        fresh, _ = controller.query("SELECT id FROM cli_t02")
        require(fresh == [[i64(1)]], "cancel changed data or kept the storage lease")
        print("Server observed the real waiting request and released it after the isolated console event", flush=True)


def test_unknown(cli, fixture, cwd, port, context, password, creds):
    script = cwd / "commit.sql"
    script.write_text("INSERT INTO cli_t02 VALUES (2,'confirmed once');\n", encoding="utf-8")
    with relay(port, context, fixture) as proxy:
        faulted = cli_args(cli, fixture, proxy.server_address[1])
        lost = run([*faulted, "--file", str(script), "--transaction", "single"], password, cwd)
        failure = errors(check(lost, UNKNOWN, creds, "Lost real COMMIT acknowledgement"))
        require(len(failure) == 1 and failure[0].get("unknown_outcome") is True,
                f"CLI did not label the COMMIT acknowledgement loss as unknown: {failure!r}")
        supplied = failure[0].get("request_token", "")
        require(type(supplied) is str and CORRELATION_TOKEN.fullmatch(supplied),
                "CLI did not emit the durable correlation token")
        require(not proxy.failure and proxy.receipt is not None, "relay did not observe a committed Server reply")
        key, lsn = proxy.receipt
        require(supplied == key, "CLI and the real Server used different commit tokens")
    normal = cli_args(cli, fixture, port)
    result = run([*normal, "--command", "\\outcome " + supplied], password, cwd)
    events = check(result, 0, creds, "Fresh CLI outcome lookup")
    resolved = {"status": "committed", "lsn": lsn}
    require([event["data"] for event in events if event["type"] == "outcome"] == [resolved],
            "fresh CLI process did not confirm the real persisted LSN")
    with client(port, context, password) as verifier:
        require(verifier.call("tx_outcome", request_token=supplied) == resolved,
                "raw Server lookup did not confirm the same newly durable LSN")
        rows, _ = verifier.query("SELECT id,note FROM cli_t02 ORDER BY id")
        require(rows == [[i64(1), text("İstanbul; JSON")], [i64(2), text("confirmed once")]],
                "independent query did not see exactly one newly committed row")
    print("Proxy, fresh CLI and raw Server confirmed one durable execution and the same commit LSN", flush=True)


def test_batch(command, cwd, password, wrong):
    creds = (password, wrong)
    ok = check(run([*command, "--command", "SELECT id,note FROM cli_t02"], password, cwd), 0, creds,
               "Successful machine batch")
    result_rows = [row for event in ok if event["type"] == "batch" for row in event["data"].get("rows", [])]
    require(result_rows == [[i64(1), text("İstanbul; JSON")]], f"unexpected typed machine result: {result_rows!r}")
    denied = errors(check(run([*command, "--command", "SELECT id FROM cli_t02"], wrong, cwd),
                          AUTH_ERROR, creds, "Invalid credentials"))
    require(len(denied) == 1 and denied[0].get("code") == 1005
            and denied[0].get("unknown_outcome") is False,
            f"CLI authentication category disagreed with the Server: {denied!r}")
    bad_sql = errors(check(run([*command, "--command", "SELECT absent_column FROM cli_t02"], password, cwd),
                           SQL_ERROR, creds, "SQL binding failure"))
    require(len(bad_sql) == 1 and type(bad_sql[0].get("code")) is int and bad_sql[0]["code"] != 0
            and re.fullmatch("[0-9A-Z]{5}", bad_sql[0].get("sqlstate", ""))
            and bad_sql[0]["sqlstate"] != "00000"
            and bad_sql[0].get("unknown_outcome") is False,
            f"CLI did not report a definite SQL error: {bad_sql!r}")


def acceptance(args):
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="cli02-", dir=args.scratch.resolve()) as directory:
        fixture = Path(directory)
        cwd = fixture / "consumer"
        cwd.mkdir()
        cli = Path(shutil.copy2(args.cli.resolve(strict=True), cwd))
        shutil.copy2(args.dll.resolve(strict=True), cwd)
        server, openssl = str(args.server.resolve(strict=True)), str(args.openssl.resolve(strict=True))
        password, wrong = secrets.token_urlsafe(32), secrets.token_urlsafe(32)
        certificate(openssl, fixture, "server")
        context = ssl.create_default_context(cafile=str(fixture / "server.pem"))
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        with sql_server(server, fixture, password) as (_, port):
            with client(port, context, password) as setup:
                write(setup, "CREATE TABLE cli_t02 (id BIGINT PRIMARY KEY, note TEXT)", 0)
                write(setup, "INSERT INTO cli_t02 VALUES (1,:note)", 1,
                      types={"note": {"type": "TEXT"}}, values={"note": text("İstanbul; JSON")})
            command = cli_args(cli, fixture, port)
            test_batch(command, cwd, password, wrong)
            test_interrupt(command, cwd, port, context, password, (password, wrong))
            test_unknown(cli, fixture, cwd, port, context, password, (password, wrong))
        checked = run_server([server, "--config", str(fixture / "server.json"), "--check-storage"], cwd=fixture)
        require(checked.returncode == 0, "fixture storage verifier failed")
    print("S36-T02 passed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("cli", "dll", "server", "openssl", "scratch"):
        parser.add_argument("--" + key, type=Path, required=True)
    parser.add_argument("--console-worker", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if os.name == "nt" and not args.console_worker:
        startup = subprocess.STARTUPINFO(dwFlags=subprocess.STARTF_USESHOWWINDOW, wShowWindow=0)
        # Creating a new console here avoids attaching or broadcasting in the
        # launcher console. Only the worker can signal its own CLI child group.
        result = subprocess.run([sys.executable, "-B", __file__, *sys.argv[1:], "--console-worker"],
                                creationflags=subprocess.CREATE_NEW_CONSOLE, startupinfo=startup, timeout=90,
                                capture_output=True, check=False)
        sys.stdout.buffer.write(result.stdout)
        sys.stderr.buffer.write(result.stderr)
        return result.returncode
    acceptance(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
