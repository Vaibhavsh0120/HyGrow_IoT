"""Run DHT, light and water-temperature drivers with invalid hardware results."""
from pathlib import Path
import runpy, tempfile, shutil, subprocess
root=Path(__file__).resolve().parent.parent
shared=runpy.run_path(str(root/'tools/test-local-telemetry.py'))
code=r'''#include "src/core/state.h"
#include <cmath>
#include <iostream>
ConfigState currentConfig{}; SensorState currentSensors{}; VitalsState currentVitals{};
float fakeTemp=24, fakeHum=60, fakeLux=300, fakeWater=22;
void webLog(uint8_t,uint8_t,const String&){}
int main(){sensor_dht_init();sensor_ds18b20_init();sensor_lux_init();float a,b;
if(!sensor_dht_read(a,b)||!sensor_ds18b20_read(a)||!sensor_lux_read(a))return 2;
for(float invalid:{INFINITY,-INFINITY,NAN}) {
fakeTemp=invalid; if(sensor_dht_read(a,b)||!std::isnan(a)||!std::isnan(b)){std::cerr<<"DHT accepts invalid temperature";return 1;} fakeTemp=24;
fakeHum=invalid; if(sensor_dht_read(a,b)){std::cerr<<"DHT accepts invalid humidity";return 1;} fakeHum=60;
fakeWater=invalid; if(sensor_ds18b20_read(a)||!std::isnan(a)){std::cerr<<"Water temperature accepts invalid sample";return 1;}
fakeLux=invalid; if(sensor_lux_read(a)||!std::isnan(a)){std::cerr<<"Light accepts invalid sample";return 1;}
}
fakeHum=101;if(sensor_dht_read(a,b))return 3;fakeHum=60;fakeTemp=81;if(sensor_dht_read(a,b))return 4;
fakeWater=126;if(sensor_ds18b20_read(a))return 5;fakeWater=-127;if(sensor_ds18b20_read(a))return 6;
fakeLux=0;if(!sensor_lux_read(a)||a!=0)return 7;
std::cout<<"Sensor invalid readings rejected; valid zero preserved.\n";
}
'''
with tempfile.TemporaryDirectory(prefix='hygrow-validity-') as folder:
 w=Path(folder);(w/'src/sensors').mkdir(parents=True);(w/'src/core').mkdir()
 names=['sensor_dht22.cpp','sensor_light.cpp','sensor_water_temp.cpp']
 for name in names:shutil.copyfile(root/'src/sensors'/name,w/'src/sensors'/name)
 shutil.copyfile(root/'src/core/state.h',w/'src/core/state.h')
 import re
 enum=re.search(r'enum SensorID\s*\{[^}]+\};',(root/'config.h').read_text()).group()
 (w/'config.h').write_text('#pragma once\nconstexpr int LOG_INFO=0,LOG_ERR=2;\n'+enum)
 (w/'Arduino.h').write_text(shared['ARDUINO']+'\n#include <cmath>\nusing std::isnan; using std::isfinite;\ninline String hostString(int n){return std::to_string(n);}\n#define String(n) hostString(n)\n')
 (w/'DHT.h').write_text('#pragma once\nextern float fakeTemp,fakeHum;constexpr int DHT22=22;struct DHT { DHT(int,int){} void begin(){} float readTemperature(){return fakeTemp;}float readHumidity(){return fakeHum;} };')
 (w/'Wire.h').write_text('#pragma once\ninline int Wire=0;')
 (w/'BH1750.h').write_text('#pragma once\nextern float fakeLux;struct BH1750{static constexpr int CONTINUOUS_HIGH_RES_MODE=1;bool begin(int,int,int*){return true;}float readLightLevel(){return fakeLux;}};')
 (w/'OneWire.h').write_text('#pragma once\nstruct OneWire{OneWire(int){}};')
 (w/'DallasTemperature.h').write_text('#pragma once\nextern float fakeWater;constexpr float DEVICE_DISCONNECTED_C=-127;struct DallasTemperature{DallasTemperature(OneWire*){}void begin(){}void requestTemperatures(){}float getTempCByIndex(int){return fakeWater;}};')
 (w/'test.cpp').write_text(code);exe=w/'test.exe'
 subprocess.run([shutil.which('g++'),'-std=c++17','-I',str(w),*[str(w/'src/sensors'/n) for n in names],str(w/'test.cpp'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
