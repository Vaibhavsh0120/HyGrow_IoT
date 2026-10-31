"""Compile the production config serializer to check credential disclosure."""
from pathlib import Path
import runpy, tempfile, shutil, re, subprocess
root=Path(__file__).resolve().parent.parent
shared=runpy.run_path(str(root/'tools/test-local-telemetry.py'))
s=(root/'src/core/task_network.cpp').read_text()
serializer=s[s.index('void broadcastConfig()\n{'):s.index('void broadcastData()')]
code=r'''#include "src/core/state.h"
#include <ArduinoJson.h>
#include <iostream>
ConfigState currentConfig{}; SensorState currentSensors{}; VitalsState currentVitals{};
struct { int count() { return 1; } } ws;
String payload;
void wsTextAllAuthed(const String& value) { payload=value; }
String auth_get_password_for_ws() { return "fixture-admin"; }
'''+serializer+r'''
int main() {
strcpy(currentConfig.wifi_pass,"fixture-wifi"); strcpy(currentConfig.ap_pass,"fixture-ap"); strcpy(currentConfig.fb_pass,"fixture-cloud");
broadcastConfig(); JsonDocument doc; if(deserializeJson(doc,payload)) return 2;
for(auto field : {"admin_pass","wifi_pass","ap_pass","fb_pass"}) if(!doc[field].isNull()) { std::cerr<<"Password disclosed: "<<field; return 1; }
std::cout<<"Config password disclosure regression passed.\n";
}
'''
with tempfile.TemporaryDirectory(prefix='hygrow-config-') as folder:
 w=Path(folder); (w/'src/core').mkdir(parents=True)
 shutil.copyfile(root/'src/core/state.h', w/'src/core/state.h')
 enum=re.search(r'enum SensorID\s*\{[^}]+\};',(root/'config.h').read_text()).group()
 (w/'config.h').write_text('#pragma once\n'+enum); (w/'Arduino.h').write_text(shared['ARDUINO']); (w/'test.cpp').write_text(code)
 exe=w/'test.exe'
 subprocess.run([shutil.which('g++'),'-std=c++17','-DARDUINOJSON_ENABLE_ARDUINO_STRING=0','-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0','-I',str(w),'-I',str(root/'.pio/libdeps/esp32-s3-n16r8/ArduinoJson/src'),str(w/'test.cpp'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
assert 'auth_get_password_for_boot_display' not in (root/'HyGrow_IoT.ino').read_text(), 'Admin password in boot logs'
