"""Use generated test credentials and an isolated Windows loopback Server."""

import argparse
from contextlib import contextmanager
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import queue
import secrets
import shutil
import subprocess
import tempfile
import threading
import time


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(command, **options):
    return subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=12, **options)


def certificate(tool, root, name):
    config = root / "req.cnf"
    config.write_text("[req]\ndistinguished_name=dn\n[dn]\n", encoding="ascii")
    answer = run([tool, "req", "-config", str(config), "-x509", "-newkey", "rsa:2048", "-nodes",
                  "-keyout", f"{name}.key", "-out", f"{name}.pem", "-days", "2", "-subj", "/CN=localhost",
                  "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
                  "-addext", "extendedKeyUsage=serverAuth"], cwd=root)
    require(answer.returncode == 0, "test certificate generation failed")


def readline(process):
    output = queue.Queue(maxsize=1)
    threading.Thread(target=lambda: output.put(process.stdout.readline()), daemon=True).start()
    try:
        return output.get(timeout=8).strip()
    except queue.Empty as error:
        raise RuntimeError("process did not become ready") from error


@contextmanager
def process(command, cwd, with_console=False):
    startup = subprocess.STARTUPINFO(dwFlags=subprocess.STARTF_USESHOWWINDOW, wShowWindow=0)
    instance = subprocess.Popen(command, cwd=cwd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace",
                                startupinfo=startup,
                                creationflags=subprocess.CREATE_NEW_CONSOLE if with_console else subprocess.CREATE_NO_WINDOW)
    try:
        yield instance
    finally:
        if instance.poll() is None:
            instance.kill()
        instance.communicate(timeout=8)


@contextmanager
def start_server(server, root, password):
    for name in ("data", "logs", "tmp"):
        (root / name).mkdir()
    config = root / "server.json"
    config.write_text(json.dumps(dict(data_dir="data", log_dir="logs", temp_dir="tmp", tls_certificate="server.pem",
                                      tls_private_key="server.key", port=0, timeout_ms=10000)), encoding="utf-8")
    command = [server, "--config", str(config)]
    initialized = run([*command, "--bootstrap-admin", "local-admin"], cwd=root, input=(password + "\n") * 2)
    require(initialized.returncode == 0, "bootstrap failed")
    with process(command, root) as instance:
        address = readline(instance).split()
        require(len(address) == 3 and address[:2] == ["LISTENING", "127.0.0.1"], "server did not bind loopback")
        yield instance, int(address[2])


def ctrl_c(pid):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.AttachConsole.argtypes = [wintypes.DWORD]
    kernel.AttachConsole.restype = wintypes.BOOL
    kernel.SetConsoleCtrlHandler.argtypes = [ctypes.c_void_p, wintypes.BOOL]
    kernel.GenerateConsoleCtrlEvent.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel.FreeConsole()
    require(kernel.AttachConsole(pid), "could not attach to the isolated CLI console")
    try:
        require(kernel.SetConsoleCtrlHandler(None, True), "could not suppress test process Ctrl+C")
        require(kernel.GenerateConsoleCtrlEvent(0, 0), "could not send Ctrl+C")
        time.sleep(0.15)
    finally:
        kernel.FreeConsole()
        kernel.SetConsoleCtrlHandler(None, False)


def secret_clear(result, secret):
    require(secret not in result.stdout + result.stderr, "password was emitted")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("cli", "dll", "server", "openssl", "scratch"):
        parser.add_argument("--" + key, type=Path, required=True)
    args = parser.parse_args()
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="cli-", dir=args.scratch.resolve()) as workspace:
        root = Path(workspace)
        client = root / "client"
        client.mkdir()
        cli = str(shutil.copy2(args.cli, client))
        shutil.copy2(args.dll, client)

        certificate(str(args.openssl), root, "server")
        certificate(str(args.openssl), root, "other")
        password = secrets.token_urlsafe(24)
        wrong = secrets.token_urlsafe(24)
        with start_server(str(args.server), root, password) as (server, port):
            profile = client / "profile.json"
            profile.write_text(json.dumps(dict(host="localhost", port=port, user="local-admin",
                                               ca_file="../server.pem", timeout_ms=4000)), encoding="utf-8")
            command = [cli, "--profile", str(profile), "--password-stdin"]
            human = run([*command, "--status"], cwd=client, input=password + "\n")
            require(human.returncode == 0 and human.stdout == f"Connected to localhost port {port}\n"
                    and not human.stderr, "isolated CLI connection failed")
            secret_clear(human, password)

            machine = run([*command, "--json", "--color", "always"], cwd=client, input=password + "\n")
            require(machine.returncode == 0 and not machine.stderr and "\x1b" not in machine.stdout,
                    "machine output contained diagnostics or ANSI")
            decoded = json.loads(machine.stdout)
            require(decoded["connected"] and decoded["port"] == port and decoded["code"] == 0
                    and decoded["capabilities"]["health"] and not decoded["capabilities"]["sql"],
                    "unexpected machine status")
            secret_clear(machine, password)

            color = run([*command, "--status", "--color", "always"], cwd=client, input=password + "\n")
            require(color.returncode == 0 and "\x1b[32m" in color.stdout, "explicit ANSI color was not enabled")

            for overrides, cred, code, product in (
                ([], wrong, 3, 1005),
                (["--ca", str(root / "other.pem")], password, 4, 1011),
                (["--tls-name", "different.invalid"], password, 4, 1011),
            ):
                result = run([*command, "--json", *overrides], cwd=client, input=cred + "\n")
                obj = json.loads(result.stdout)
                require(result.returncode == code and not obj["connected"] and obj["code"] == product
                        and result.stderr and "\x1b" not in result.stdout + result.stderr,
                        "TLS or authentication error had an unexpected status")
                secret_clear(result, cred)

            invalid = client / "invalid.json"
            invalid.write_text(json.dumps({"password": wrong}), encoding="utf-8")
            rejected = run([cli, "--profile", str(invalid), "--json", "--password-stdin"], cwd=client, input="")
            require(rejected.returncode == 2 and not rejected.stdout, "secret-bearing profile was accepted")
            secret_clear(rejected, wrong)

            session = run(command, cwd=client, input=password + "\n\\capabilities\n\\quit\n")
            require(session.returncode == 0 and "sql unavailable" in session.stdout and "health available" in session.stdout
                    and "\\help" not in session.stdout and "\x1b" not in session.stdout and not session.stderr,
                    "normal pipe session contained a prompt, banner or wrong capability")
            sql = run(command, cwd=client, input=password + "\nSELECT 1\n")
            require(sql.returncode == 5 and "SQL is unavailable" in sql.stderr and "SELECT" not in sql.stdout,
                    "SQL did not preserve the unavailable status at EOF")

            with process(command, client, with_console=True) as cli_process:
                cli_process.stdin.write(password + "\n")
                cli_process.stdin.flush()
                require(readline(cli_process).startswith("Connected"), "Ctrl+C fixture did not connect")
                ctrl_c(cli_process.pid)
                cli_process.communicate(timeout=6)
                require(cli_process.returncode == 6, "Ctrl+C failed to close the client")

            with process(command, client) as cli_process:
                cli_process.stdin.write(password + "\n")
                cli_process.stdin.flush()
                require(readline(cli_process).startswith("Connected"), "connection-loss fixture did not connect")
                server.kill()
                server.wait(timeout=6)
                _, errors = cli_process.communicate("\\status\n", timeout=8)
                require(cli_process.returncode == 4 and "SDP 1010" in errors, "connection loss was not reported")

        require(not (client / "StarDataParadiseServer.exe").exists(), "client fixture included the Server EXE")
    print("isolated TLS, DNS, profile, JSON, color, rejection, SQL unavailable, Ctrl+C, EOF and connection-loss passed")


if __name__ == "__main__":
    main()
