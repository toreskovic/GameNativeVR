"""Run the CRT-free pose-time test under Wine against a mock headset control server."""
import os
from pathlib import Path
import socketserver
import subprocess
import threading

work = Path(__file__).resolve().parents[2] / "app/build/windows-xr-runtime-linux"
commands = []
class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        for line in self.rfile:
            command = line.decode().strip()
            commands.append(command)
            response = "OK"
            if command.startswith("LOCATE_VIEWS"):
                n = sum(c.startswith("LOCATE_VIEWS") for c in commands)
                response = f"OK flags=1 lpx={n * 1000000} lqw=1000000 rqw=1000000"
                if command == "LOCATE_VIEWS time=3000000000":
                    response = "ERROR unavailable"
            self.wfile.write((response + "\n").encode())
            self.wfile.flush()
class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
with Server(("127.0.0.1", 38476), Handler) as server:
    threading.Thread(target=server.serve_forever, daemon=True).start()
    env = dict(os.environ, WINEPREFIX=str(work / "wine-test-prefix"), WINEDEBUG="-all")
    result = subprocess.run(["wine", "pose-time-smoke.exe"], cwd=work, env=env, timeout=55)
    assert result.returncode == 0, (result.returncode, commands)
    assert [c for c in commands if c.startswith("LOCATE_VIEWS")] == [
        "LOCATE_VIEWS time=1000000000", "LOCATE_VIEWS time=1000000000",
        "LOCATE_VIEWS time=2000000000", "LOCATE_VIEWS time=2500000000",
        "LOCATE_VIEWS time=3000000000",
    ], commands
    print("Requested-time queries, late refresh, validity flags, and failure handling passed")
