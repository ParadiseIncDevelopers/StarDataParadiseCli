"""CLI Stage 36 management and detached jobs against an isolated real TLS Server."""

import argparse
from contextlib import contextmanager
import json
import math
from pathlib import Path
import re
import secrets
import shutil
import tempfile
import time

from cli_smoke import certificate, process, readline, require, run, secret_clear


@contextmanager
def sql_server(server, root, password):
    for name in ("data", "logs", "tmp", "backups"):
        (root / name).mkdir()
    config = root / "server.json"
    config.write_text(json.dumps({"data_dir": "data", "log_dir": "logs", "temp_dir": "tmp",
                                  "tls_certificate": "server.pem", "tls_private_key": "server.key",
                                  "port": 0, "timeout_ms": 15_000, "max_connections": 8, "backup": "backups"}), encoding="utf-8")
    command = [server, "--config", str(config)]
    initialized = run([*command, "--initialize-storage"], cwd=root)
    require(initialized.returncode == 0, "Could not initialize SQL storage in the test fixture")
    bootstrap = run([*command, "--bootstrap-admin", "local-admin"], cwd=root, input=(password + "\n") * 2)
    secret_clear(bootstrap, password)
    require(bootstrap.returncode == 0, "Could not bootstrap the test administrator")
    with process(command, root) as instance:
        address = readline(instance).split()
        require(len(address) == 3 and address[:2] == ["LISTENING", "127.0.0.1"], "Server did not bind loopback")
        yield instance, int(address[2])


def i64(value):
    return {"t": "i64", "v": str(value)}


def text(value):
    return {"t": "text", "v": value}


def rpc_line(group, operation, **fields):
    return "\\rpc " + group + " " + operation + " " + json.dumps(fields, separators=(",", ":"))


def events(document, kind):
    return [event["data"] for event in document["events"] if event["type"] == kind]


def rows(document):
    return [row for batch in events(document, "batch") for row in batch["rows"]]


def send_event(cli, password, command, kind):
    cli.stdin.write(command + "\n")
    cli.stdin.flush()
    for _ in range(16):
        line = readline(cli)
        require(bool(line) and "\x1b" not in line and password not in line, f"CLI did not stream a safe {kind} event")
        event = json.loads(line)
        require(event["type"] != "error", f"Streamed command rejected: {event['data']}")
        if event["type"] == kind:
            return event["data"]
    raise AssertionError(f"CLI did not flush a streamed {kind} event")


def finish_stream(cli, password, expected=0):
    trailing, errors = cli.communicate("\\quit\n" if expected == 0 else None, timeout=8)
    require(cli.returncode == expected and password not in trailing + errors and "\x1b" not in trailing + errors,
            f"Expected streamed CLI exit {expected}, received {cli.returncode}")
    trailer = [json.loads(line) for line in trailing.splitlines() if line]
    require(trailer and trailer[-1] == {"type": "status", "data": {"exit_code": expected}},
            "NDJSON stream did not close with its real exit status")
    return trailer, errors


class Consumer:
    def __init__(self, cli, profile, work, password):
        self.base = [str(cli), "--profile", str(profile), "--password-stdin", "--color", "always"]
        self.work = work
        self.password = password

    def run(self, *args, expected=0, password=None):
        secret = self.password if password is None else password
        result = run([*self.base, "--format", "json", *args], cwd=self.work, input=secret + "\n")
        secret_clear(result, secret)
        require("\x1b" not in result.stdout + result.stderr, "ANSI leaked into machine mode")
        require(result.returncode == expected,
                f"Expected exit {expected}, received {result.returncode}: {result.stderr}")
        try:
            body = json.loads(result.stdout)
        except json.JSONDecodeError as error:
            raise AssertionError("CLI did not emit exactly one JSON document") from error
        require(isinstance(body.get("events"), list) and body.get("exit_code") == expected,
                "CLI document events/exit trailer did not match the process")
        return body

    def sql(self, command, expected=0):
        return self.run("--command", command, expected=expected)

    def rpc(self, group, operation, *, expected=0, **fields):
        document = self.sql(rpc_line(group, operation, **fields), expected=expected)
        receipts = events(document, group)
        require(bool(receipts), f"{group} {operation} omitted its native receipt")
        return receipts[-1]

    def stream_history_restore(self, target, revision, expected):
        # The Server owns the plan for the authenticated session that previewed it.
        with process([*self.base, "--format", "ndjson"], self.work) as cli:
            cli.stdin.write(self.password + "\n")
            plan = send_event(cli, self.password, rpc_line("history", "restore", target=target, revision=revision), "history")
            require(plan["revision"] == revision and plan["effects"]["inserted"] == 1 and plan["plan_id"],
                    "Restore preview did not identify the deleted row and source revision")
            restored = send_event(cli, self.password, rpc_line("history", "restore", plan_id=plan["plan_id"]), "history")
            require(restored["status"] == "committed" and restored["replayed"] is False and
                    int(restored["lsn"]) > int(plan["current_revision"]), "History was not restored and committed")
            observed = send_event(cli, self.password, f"SELECT id,phase FROM {target} ORDER BY id;", "batch")
            require(observed["rows"] == expected, "Restored rows were not visible in the same session")
            _, errors = finish_stream(cli, self.password)
            require(not errors, "The successful restore session exited with a diagnostic")


def management(cli):
    path = cli.work / "scenario.sql"
    path.write_text("""CREATE TABLE cli_orders (id BIGINT PRIMARY KEY, phase TEXT NOT NULL);
INSERT INTO cli_orders VALUES (1,'pending;review'),(2,'retained'),(3,'temporary');
-- This is a comment with a semicolon ;
UPDATE cli_orders SET phase='ready' WHERE id=1;
DELETE FROM cli_orders WHERE id=3;
CREATE FUNCTION cli_bonus(item IN BIGINT) RETURN BIGINT AS
BEGIN
    IF item = 9 THEN
        RETURN item * 3;
    END IF;
    RETURN 0;
END;
SELECT id,phase FROM cli_orders ORDER BY id;
SELECT cli_bonus(9);
""", encoding="utf-8")
    document = cli.run("--file", str(path))
    retained = [[i64(1), text("ready")], [i64(2), text("retained")]]
    require(rows(document) == retained + [[i64(27)]], "The CLI file corrupted CRUD or multiline PL/SQL")
    counts = [event["affected_rows"] for event in events(document, "execute") if "result_id" not in event]
    require(counts == [0, 3, 1, 1, 0], f"The CLI did not report each affected-row receipt: {counts}")

    revision = cli.rpc("history", "status")["current_revision"]
    require(isinstance(revision, str) and int(revision) > 0, "History did not report a committed revision")
    cli.sql("DELETE FROM cli_orders WHERE id=2")
    require(rows(cli.sql("SELECT id,phase FROM cli_orders")) == retained[:1], "History fixture deletion failed")
    cli.stream_history_restore("cli_orders", revision, retained)
    require(rows(cli.sql("SELECT id,phase FROM cli_orders ORDER BY id")) == retained,
            "History restore did not survive connection close")
    require(cli.rpc("history", "verify")["verified"] is True, "Restored History failed verification")

    cli.sql("CREATE DECISION RULE cli_protect ON cli_orders FOR UPDATE PRIORITY 100 "
            "WHEN (NEW.phase = 'forbidden') THEN DENY")
    before = cli.rpc("history", "status")["current_revision"]
    denied = cli.sql("UPDATE cli_orders SET phase='forbidden' WHERE id=1", expected=3)
    require(any(error.get("code") == 1006 for error in events(denied, "error")),
            "Decision refusal did not surface as a Server permission denial")
    require(rows(cli.sql("SELECT id,phase FROM cli_orders ORDER BY id")) == retained and
            cli.rpc("history", "status")["current_revision"] == before,
            "Decision-denied write changed persisted rows or published History")
    cli.sql("SELECT * FROM cli_missing_table", expected=5)
    cli.run("--command", "SELECT 1", password=secrets.token_urlsafe(31), expected=3)

    cli.sql("CREATE TABLE cli_training (sample_key BIGINT PRIMARY KEY, reading BIGINT, outcome_value BIGINT)")
    samples = ",".join(f"({index},{value},{3 * value + 7})" for index, value in enumerate(range(-15, 15), 1))
    cli.sql("INSERT INTO cli_training VALUES " + samples)
    dataset = cli.rpc("dataset", "create", sql="SELECT reading,outcome_value FROM cli_training ORDER BY sample_key",
                      options={"features": [{"column": "reading", "transform": "numeric", "impute": "reject"}],
                               "target": "outcome_value", "target_kind": "numeric",
                               "split": {"mode": "random", "seed": 23, "train": 60, "validation": 20}})
    data_id = dataset["dataset_id"]
    require(dataset["row_count"] == 30 and cli.rpc("dataset", "get", dataset_id=data_id)["row_count"] == 30,
            "Typed dataset consumed the wrong quantity of SQL rows")
    cli.rpc("model", "create", name="cli_forecast", algorithm="linear")
    training = cli.rpc("model", "train", name="cli_forecast", dataset_id=data_id, seed=19)
    done = cli.rpc("job", "watch", job_id=training["job_id"])
    artifact = done["result"]
    require(done["state"] == "succeeded" and done["attempt"] == 1 and artifact["dataset_id"] == data_id and
            re.fullmatch("[a-f0-9]{64}", artifact["checksum"]), "Model training did not publish the expected artifact")
    cli.rpc("model", "activate", name="cli_forecast", version=artifact["version"])
    prediction = cli.rpc("model", "predict", name="cli_forecast", features={"reading": 17})
    require(prediction["version"] == artifact["version"] and math.isclose(prediction["prediction"], 58, abs_tol=1e-4),
            "Activated CLI model did not produce the trained prediction")

    backup_job = cli.rpc("backup", "submit")["job_id"]
    backup = cli.rpc("job", "watch", job_id=backup_job)
    archive = backup["result"]
    require(backup["state"] == "succeeded" and archive["verified"] is True and
            re.fullmatch("[a-f0-9]{64}", archive["manifest_sha256"]),
            "Backup watcher did not return a verified archive")
    status = cli.rpc("backup", "status", job_id=backup_job)
    verified = cli.rpc("backup", "verify", backup_id=archive["backup_id"])
    require(status["result"] == archive and verified["verified"] is True,
            "Backup status or independent verification disagreed with the scheduler")
    print("S36-T03 passed: file CRUD, routine, same-session History restore, Decision deny, trained model and verified backup", flush=True)


def detached_jobs(cli, worker_temp):
    command = [*cli.base, "--format", "ndjson"]
    with process(command, cli.work) as monitor, process(command, cli.work) as origin:
        for connected in (monitor, origin):
            connected.stdin.write(cli.password + "\n")
            ready = send_event(connected, cli.password, "SELECT 1;", "batch")
            require(ready["rows"] == [[i64(1)]], "Job test connection did not authenticate")

        def control(instance, operation, **fields):
            return send_event(instance, cli.password, rpc_line("job", operation, **fields), "job")

        blocked = control(monitor, "submit", kind="compute", count=1_000_000_000, timeout_ms=60_000)
        blocker_id = blocked["job_id"]
        deadline = time.monotonic() + 8
        live = None
        while time.monotonic() < deadline:
            live = control(monitor, "status", job_id=blocker_id)
            if live["state"] == "running" and isinstance(live["worker_pid"], int):
                break
            require(live["state"] in {"queued", "starting", "running"}, f"Job blocker ended early: {live['state']}")
            time.sleep(0.02)
        require(live is not None and live["state"] == "running" and isinstance(live["worker_pid"], int),
                "CLI could not observe a running worker to cancel")
        worker_pid = live["worker_pid"]

        count = 2_500_000
        submitted = control(origin, "submit", kind="compute", count=count)
        queued_id = submitted["job_id"]
        _, errors = finish_stream(origin, cli.password)
        require(not errors, "The submitting CLI did not close cleanly")
        queued = control(monitor, "status", job_id=queued_id)
        require(queued["state"] == "queued" and queued["worker_pid"] is None,
                f"Detached job already left the queue before the proof: {queued['state']}")

        cancellation = control(monitor, "cancel", job_id=blocker_id)
        require(cancellation["cancel_requested"] is True, "CLI cancel lost the durable request")
        complete = control(monitor, "watch", job_id=queued_id)
        states = [event["state"] for event in complete["events"]]
        require(complete["state"] == "succeeded" and complete["attempt"] == 1 and
                complete["result"]["count"] == count and complete["result"]["checksum"].isdigit() and
                states == ["queued", "starting", "running", "succeeded"],
                "The detached job did not complete the exact computation on the released worker")
        cancelled = control(monitor, "watch", job_id=blocker_id)
        require(cancelled["state"] == "cancelled" and cancelled["worker_pid"] is None and
                cancelled["result"] is None and cancelled["reason"] == "cancelled",
                f"Cancelled worker {worker_pid} retained a live resource or published a result")
        trailer, _ = finish_stream(monitor, cli.password, expected=6)
        require(any(event["type"] == "error" and event["data"]["code"] == 1009 for event in trailer),
                "Cancelled job did not emit its machine-readable cancellation code")

    deadline = time.monotonic() + 4
    while any(worker_temp.iterdir()) and time.monotonic() < deadline:
        time.sleep(0.035)
    require(not any(worker_temp.iterdir()), "Cancelled or completed job leaked worker scratch")
    require(rows(cli.sql("SELECT 8")) == [[i64(8)]], "Job cancellation broke continued SQL service")
    print("S36-T04 passed: durable queued job completed after submitter exit, cancel exit 6 and worker scratch released", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("cli", "dll", "server", "openssl", "scratch"):
        parser.add_argument("--" + key, type=Path, required=True)
    parser.add_argument("--case", choices=("all", "management", "jobs"), default="all")
    args = parser.parse_args()
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="m8-cli-", dir=args.scratch.resolve()) as directory:
        root = Path(directory)
        installed = root / "client"
        installed.mkdir()
        cli = Path(shutil.copy2(args.cli.resolve(strict=True), installed))
        shutil.copy2(args.dll.resolve(strict=True), installed)
        certificate(str(args.openssl.resolve(strict=True)), root, "server")
        password = secrets.token_urlsafe(32)
        with sql_server(str(args.server.resolve(strict=True)), root, password) as (_, port):
            profile = installed / "fixture-profile.json"
            profile.write_text(json.dumps({"host": "localhost", "port": port, "user": "local-admin",
                                           "ca_file": "../server.pem", "timeout_ms": 9000}), encoding="utf-8")
            consumer = Consumer(cli, profile, installed, password)
            if args.case in {"all", "management"}:
                management(consumer)
            if args.case in {"all", "jobs"}:
                detached_jobs(consumer, root / "tmp")
        require(not (installed / "StarDataParadiseServer.exe").exists(), "Client fixture included the Server EXE")


if __name__ == "__main__":
    main()
