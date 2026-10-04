import os,socketserver,threading,time,subprocess
from pathlib import Path

repository = Path(__file__).resolve().parents[2]
work = repository / "app/build/windows-xr-runtime-linux"
prefix = work / "wine-test-prefix"
prefix.mkdir(parents=True, exist_ok=True)
commands=[]
revision=2
changed=False
class Handler(socketserver.StreamRequestHandler):
 def handle(self):
  global revision,changed
  for line in self.rfile:
   command=line.decode().strip();commands.append(command)
   response=f'OK time={time.monotonic_ns()+123456789000000}' if command=='GET_TIME' else 'OK'
   if command.startswith('GET_VISIBILITY_MASK '):
    _,eye,kind,offset=command.split();offset=int(offset)
    if offset==64 and not changed: revision=3;changed=True
    values=[-500000,250000]*40+list(range(40))*3 if eye=='0' else []
    response=f'OK revision={revision} vertices={40 if values else 0} indices={120 if values else 0}'
    response+=''.join(f' v{i}={v}' for i,v in enumerate(values[offset:offset+64]))
   self.wfile.write((response+'\n').encode());self.wfile.flush()
class Server(socketserver.ThreadingTCPServer):
 allow_reuse_address=True
 daemon_threads=True
with Server(('127.0.0.1',38476),Handler) as server:
 threading.Thread(target=server.serve_forever,daemon=True).start()
 env=dict(os.environ,WINEPREFIX=str(prefix),WINEDEBUG='-all')
 result=subprocess.run(['wine','visibility-smoke.exe'],cwd=work,env=env,timeout=55)
 print('Windows runtime smoke exit:',result.returncode,'commands:',commands)
 assert result.returncode==0
 assert commands.count('GET_TIME')==5, commands
