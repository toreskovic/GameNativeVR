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
            if command.startswith("LOCATE_HAND"):
                n = sum(c.startswith("LOCATE_HAND") for c in commands)
                response = f"OK flags=15 velocityFlags=3 qw=1000000 px={n * 1000000} vx=1000000 wz=2000000"
                if "time=4200000000" in command:
                    response = "OK flags=1 velocityFlags=2 qw=1000000"
                if "time=4300000000" in command:
                    response = "OK flags=0 velocityFlags=0"
                if "time=4400000000" in command:
                    response = "ERROR unavailable"
            if command == "FRAME_SYNC":
                n = commands.count(command)
                response = "ERROR gpu_busy" if n == 1 else "OK serial=1 time=5000000000 period=11111111 render=1\nOK flags=1\nOK\nOK"
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
    assert [c for c in commands if c.startswith("LOCATE_HAND")] == [
        "LOCATE_HAND time=4000000000 hand=0 aim=0",
        "LOCATE_HAND time=4000000000 hand=0 aim=0",
        "LOCATE_HAND time=4100000000 hand=1 aim=1",
        "LOCATE_HAND time=4200000000 hand=1 aim=1",
        "LOCATE_HAND time=4300000000 hand=1 aim=1",
        "LOCATE_HAND time=4400000000 hand=1 aim=1",
    ], commands
    print("Head/controller requested-time queries, late refresh, offsets, velocities, action-state isolation and failure handling passed")
