"""Stress production authenticated-client membership across concurrent callbacks."""
from pathlib import Path
import tempfile, subprocess, shutil
root=Path(__file__).resolve().parent.parent
s=(root/'src/core/auth.cpp').read_text()
a=s.index('bool wsClientIsAuthed('); b=s.index('// Sends the very first frame',a)
code='#include <set>\n#include <mutex>\n#include <cstdint>\n#include <thread>\n#include <vector>\nstatic std::set<uint32_t> s_authedClients;\nstatic std::mutex s_authedClientsMutex;\n'+s[a:b]+r'''
int main(){ std::vector<std::thread> workers;
for(unsigned i=0;i<8;i++) workers.emplace_back([i]{for(unsigned n=0;n<10000;n++){wsMarkClientAuthed(i+1); wsClientIsAuthed(i+1); wsForgetClient(i+1);}});
for(auto& t:workers)t.join();
for(unsigned i=1;i<=8;i++) if(wsClientIsAuthed(i))return 1;
}
'''
with tempfile.TemporaryDirectory(prefix='hygrow-auth-') as folder:
 p=Path(folder); (p/'test.cpp').write_text(code);exe=p/'test.exe'
 subprocess.run([shutil.which('g++'),'-std=c++17','-pthread',str(p/'test.cpp'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=30)
print('Concurrent authenticated-client membership passed.')
