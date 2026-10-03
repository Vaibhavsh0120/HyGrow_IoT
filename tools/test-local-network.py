"""Run production HTTP/UDP handlers with simulated ESP32 network interfaces."""
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import tempfile

UDP = r"""
#pragma once
#include <Arduino.h>
#include <functional>
#include <vector>
constexpr int TCPIP_ADAPTER_IF_STA=0, TCPIP_ADAPTER_IF_AP=1;
struct IPAddress {
    uint32_t native;
    IPAddress(uint32_t value=0) : native(value) {}
    uint8_t operator[](int n) const { return (native >> (n*8)) & 255; }
    String toString() const { return std::to_string((*this)[0])+"."+std::to_string((*this)[1])+"."+std::to_string((*this)[2])+"."+std::to_string((*this)[3]); }
};
extern String fakeReply;
extern unsigned fakeWrites, fakeListens;
extern bool fakeListenOk;
struct AsyncUDPPacket {
    std::vector<uint8_t> bytes;
    IPAddress peer;
    int iface;
    bool isIPv6() { return false; }
    uint16_t remotePort() { return 43210; }
    size_t length() { return bytes.size(); }
    int interface() { return iface; }
    IPAddress remoteIP() { return peer; }
    uint8_t* data() { return bytes.data(); }
    size_t write(const uint8_t* data, size_t size) { fakeReply.assign(reinterpret_cast<const char*>(data),size); ++fakeWrites; return size; }
};
extern std::function<void(AsyncUDPPacket)> fakeOnPacket;
struct AsyncUDP {
    void onPacket(std::function<void(AsyncUDPPacket)> fn) { fakeOnPacket=fn; }
    bool listen(uint16_t port) { ++fakeListens; return port == 39400 && fakeListenOk; }
};
"""
NETIF = r"""
#pragma once
#include <cstdint>
constexpr int ESP_OK=0;
struct Ip { uint32_t addr; };
struct esp_netif_ip_info_t { Ip ip, netmask; };
struct esp_netif_t { esp_netif_ip_info_t info; };
extern esp_netif_t fakeSTA, fakeAP;
inline esp_netif_t* esp_netif_get_handle_from_ifkey(const char* key) { return strcmp(key,"WIFI_AP_DEF") == 0 ? &fakeAP : &fakeSTA; }
inline int esp_netif_get_ip_info(esp_netif_t* iface, esp_netif_ip_info_t* out) { *out=iface->info; return ESP_OK; }
"""
HTTP = r"""
#pragma once
#include <Arduino.h>
#include <functional>
#include <map>
constexpr int HTTP_GET=1, HTTP_OPTIONS=2;
struct AsyncWebServerResponse {
    int code;
    String type, payload;
    std::map<String,String> headers;
    void addHeader(const String& key,const String& value) { headers[key]=value; }
};
struct AsyncWebServerRequest {
    AsyncWebServerResponse* response=nullptr;
    ~AsyncWebServerRequest() { delete response; }
    AsyncWebServerResponse* beginResponse(int code,const String& type="",const String& payload="") { return new AsyncWebServerResponse{code,type,payload,{}}; }
    void send(AsyncWebServerResponse* result) { response=result; }
};
struct AsyncWebServer {
    std::map<int,std::function<void(AsyncWebServerRequest*)>> routes;
    void on(const String& path,int method,std::function<void(AsyncWebServerRequest*)> callback) { if(path=="/status") routes[method]=callback; }
};
"""
TEST = r"""
#include "src/core/local_network.h"
#include "src/core/telemetry.h"
#include <AsyncUDP.h>
#include <ESPAsyncWebServer.h>
#include <esp_netif.h>
#include <iostream>
#include <cstdlib>
ConfigState currentConfig{};
SensorState currentSensors{};
VitalsState currentVitals{};
uint32_t nowMs=1000;
uint32_t millis() { return nowMs; }
int64_t esp_timer_get_time() { return int64_t(nowMs)*1000; }
uint32_t esp_random() { return 1; }
void webLog(uint8_t,uint8_t,const String&) {}
String fakeReply;
unsigned fakeWrites=0, fakeListens=0;
bool fakeListenOk=false;
std::function<void(AsyncUDPPacket)> fakeOnPacket;
esp_netif_t fakeSTA{{{0x2a00a8c0},{0x00ffffff}}}, fakeAP{{{0x0104a8c0},{0x00ffffff}}};
static void require(bool ok,const char* msg) { if(!ok) { std::cerr<<msg<<'\n'; std::exit(1); } }
static void probe(int iface,uint32_t source=0x0500a8c0,const String& body=R"({"type":"hygrow_discover","version":1,"requestId":"n"})") {
    nowMs+=250;
    fakeOnPacket(AsyncUDPPacket{{body.begin(),body.end()},IPAddress(source),iface});
}
int main() {
    strcpy(currentConfig.device_id,"grow-01");
    currentConfig.interval_read_ms=2000;
    currentConfig.sensor_enabled[S_TDS]=true;
    currentSensors.tds_ppm=950.25;
    currentSensors.last_ok_ms[S_TDS]=1000;
    bool demo[S_COUNT]{};
    telemetryInit(); telemetryPublish(demo);
    AsyncWebServer server;
    localNetworkInit(server);
    require(fakeListens==1 && bool(fakeOnPacket),"Local listener was not registered");
    localNetworkLoop(); require(fakeListens==1,"Bind retry spun continuously");
    nowMs+=5000; fakeListenOk=true; localNetworkLoop();
    require(fakeListens==2,"Failed bind was not recovered");
    nowMs+=5000; localNetworkLoop(); require(fakeListens==2,"Healthy listener was repeatedly rebound");
    AsyncWebServerRequest get;
    server.routes[HTTP_GET](&get);
    require(get.response->code==200 && get.response->type=="application/json","GET /status did not return JSON");
    require(get.response->headers["Cache-Control"].find("no-store")!=String::npos,"Status may be cached");
    require(get.response->headers["Access-Control-Allow-Origin"]=="*","Read-only status CORS missing");
    JsonDocument status;
    require(!deserializeJson(status,get.response->payload) && status["tds_ppm"]==950.25,"HTTP handler bypassed snapshot");
    AsyncWebServerRequest options; server.routes[HTTP_OPTIONS](&options);
    require(options.response->code==204,"Status preflight failed");
    JsonDocument reply;
    probe(TCPIP_ADAPTER_IF_STA);
    require(fakeWrites==1 && !deserializeJson(reply,fakeReply),"Valid LAN probe did not receive JSON");
    require(reply["ip"]=="192.168.0.42","Station discovery advertised wrong IP");
    fakeSTA.info.ip.addr=0x4d00a8c0;
    probe(TCPIP_ADAPTER_IF_STA); deserializeJson(reply,fakeReply);
    require(reply["ip"]=="192.168.0.77","DHCP change left stale advertised IP");
    probe(TCPIP_ADAPTER_IF_AP,0x0504a8c0); deserializeJson(reply,fakeReply);
    require(reply["ip"]=="192.168.4.1","AP+STA discovery returned inaccessible station IP");
    const auto accepted=fakeWrites;
    probe(TCPIP_ADAPTER_IF_STA,0x0501a8c0);
    probe(TCPIP_ADAPTER_IF_STA,0x0500a8c0,"unrelated");
    probe(TCPIP_ADAPTER_IF_STA,0x0500a8c0,R"({"type":"hygrow_discover","version":1,"requestId":"n","deviceId":"other"})");
    require(fakeWrites==accepted,"Routed, malformed or nonmatching probe received reply");
    probe(TCPIP_ADAPTER_IF_STA);
    const String body=R"({"type":"hygrow_discover","version":1,"requestId":"n"})";
    fakeOnPacket(AsyncUDPPacket{{body.begin(),body.end()},IPAddress(0x0500a8c0),TCPIP_ADAPTER_IF_STA});
    require(fakeWrites==accepted+1,"Burst rate limit failed");
    std::cout<<"HTTP status/cache/CORS, UDP binding recovery, station/AP replies, DHCP changes, filtering and rate limits passed.\n";
}
"""

def main():
    compiler=shutil.which("g++")
    if not compiler:
        raise SystemExit("Install a host g++ compiler to run local network checks.")
    root=Path(__file__).resolve().parent.parent
    shared=runpy.run_path(str(root/"tools/test-local-telemetry.py"))
    sources=["telemetry.cpp","telemetry_json.cpp","local_protocol.cpp","local_network.cpp"]
    with tempfile.TemporaryDirectory(prefix="hygrow-network-") as folder:
        work=Path(folder); core=work/"src/core"; core.mkdir(parents=True)
        for name in sources+["state.h","telemetry.h","local_protocol.h","local_network.h"]:
            shutil.copyfile(root/"src/core"/name,core/name)
        arduino=shared["ARDUINO"]+'\nuint32_t millis();\ninline String hostString(unsigned n) { return std::to_string(n); }\n#define String(n) hostString(n)\n'
        (work/"Arduino.h").write_text(arduino)
        (work/"AsyncUDP.h").write_text(UDP)
        (work/"ESPAsyncWebServer.h").write_text(HTTP)
        (work/"esp_netif.h").write_text(NETIF)
        (work/"WiFi.h").write_text("#pragma once\n")
        (work/"esp_timer.h").write_text("#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n")
        (work/"esp_system.h").write_text("#pragma once\n#include <cstdint>\nuint32_t esp_random();\n")
        enum=re.search(r"enum SensorID\s*\{[^}]+\};",(root/"config.h").read_text()).group()
        (work/"config.h").write_text("#pragma once\nconstexpr int LOG_INFO=0, LOG_WARN=1;\n"+enum)
        (work/"test.cpp").write_text(TEST)
        executable=work/"network-test.exe"
        subprocess.run([compiler,"-std=c++17","-Wall","-Wextra","-pthread",
                        "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0","-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                        "-I",str(work),"-I",str(root/".pio/libdeps/esp32-s3-n16r8/ArduinoJson/src"),
                        *[str(core/name) for name in sources],str(work/"test.cpp"),"-o",str(executable)],check=True)
        subprocess.run([str(executable)],check=True)

if __name__=="__main__":
    main()
