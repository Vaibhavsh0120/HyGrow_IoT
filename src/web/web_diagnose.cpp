#include "web_diagnose.h"

#if ENABLE_WEB_DIAGNOSE

WebServer server(80);
static SensorData currentData;

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Sensor Diagnostic Dashboard</title>
<style>
  :root { --bg: #0f172a; --card-bg: rgba(30, 41, 59, 0.7); --text: #f8fafc; --accent: #3b82f6; --error: #ef4444; --success: #10b981; }
  body { margin: 0; font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: var(--bg); color: var(--text); padding: 2rem; }
  .dashboard { max-width: 1200px; margin: 0 auto; }
  .header { text-align: center; margin-bottom: 2rem; }
  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 1.5rem; }
  .card { background: var(--card-bg); backdrop-filter: blur(10px); border-radius: 1rem; padding: 1.5rem; border: 1px solid rgba(255,255,255,0.1); transition: transform 0.2s; box-shadow: 0 4px 6px rgba(0,0,0,0.1); }
  .card:hover { transform: translateY(-5px); }
  .card-title { font-size: 1.1rem; color: #94a3b8; margin-bottom: 1rem; display: flex; justify-content: space-between; align-items: center; }
  .status-dot { width: 12px; height: 12px; border-radius: 50%; background: var(--success); }
  .status-dot.error { background: var(--error); box-shadow: 0 0 10px var(--error); }
  .value { font-size: 2.5rem; font-weight: bold; margin: 0.5rem 0; }
  .unit { font-size: 1rem; color: #64748b; font-weight: normal; }
  .details { font-size: 0.9rem; color: #94a3b8; }
</style>
</head>
<body>
<div class="dashboard">
  <div class="header">
    <h1>Sensor Diagnostic Dashboard</h1>
    <p>Real-time telemetry and health status</p>
  </div>
  <div class="grid">
    <div class="card">
      <div class="card-title">Water Level <div id="status-wl" class="status-dot"></div></div>
      <div class="value"><span id="val-wl-pct">--</span><span class="unit">%</span></div>
      <div class="details">Raw ADC: <span id="val-wl-raw">--</span></div>
    </div>
    <div class="card">
      <div class="card-title">Ambient Light <div id="status-light" class="status-dot"></div></div>
      <div class="value"><span id="val-light">--</span><span class="unit"> lux</span></div>
    </div>
    <div class="card">
      <div class="card-title">Water TDS <div id="status-tds" class="status-dot"></div></div>
      <div class="value"><span id="val-tds">--</span><span class="unit"> ppm</span></div>
    </div>
    <div class="card">
      <div class="card-title">Air Temp & Humidity <div id="status-dht" class="status-dot"></div></div>
      <div class="value"><span id="val-air-temp">--</span><span class="unit"> °C</span></div>
      <div class="details">Humidity: <span id="val-hum">--</span>% | VPD: <span id="val-vpd">--</span> kPa</div>
    </div>
    <div class="card">
      <div class="card-title">Water pH <div id="status-ph" class="status-dot"></div></div>
      <div class="value"><span id="val-ph">--</span><span class="unit"> pH</span></div>
    </div>
    <div class="card">
      <div class="card-title">Water Temp <div id="status-wtemp" class="status-dot"></div></div>
      <div class="value"><span id="val-wtemp">--</span><span class="unit"> °C</span></div>
    </div>
  </div>
</div>
<script>
  function updateData() {
    fetch('/api/data')
      .then(r => r.json())
      .then(d => {
        document.getElementById('val-wl-pct').innerText = d.waterLevelPercent.toFixed(1);
        document.getElementById('val-wl-raw').innerText = d.waterLevelRaw;
        document.getElementById('status-wl').className = d.err[0] ? 'status-dot error' : 'status-dot';
        
        document.getElementById('val-light').innerText = d.lightLux.toFixed(1);
        document.getElementById('status-light').className = d.err[1] ? 'status-dot error' : 'status-dot';
        
        document.getElementById('val-tds').innerText = d.tdsPPM.toFixed(1);
        document.getElementById('status-tds').className = d.err[2] ? 'status-dot error' : 'status-dot';
        
        document.getElementById('val-air-temp').innerText = d.airTempC.toFixed(1);
        document.getElementById('val-hum').innerText = d.humidityPercent.toFixed(1);
        document.getElementById('val-vpd').innerText = d.vpdKpa.toFixed(2);
        document.getElementById('status-dht').className = d.err[3] ? 'status-dot error' : 'status-dot';
        
        document.getElementById('val-ph').innerText = d.phValue.toFixed(2);
        document.getElementById('status-ph').className = d.err[4] ? 'status-dot error' : 'status-dot';
        
        document.getElementById('val-wtemp').innerText = d.waterTempC.toFixed(1);
        document.getElementById('status-wtemp').className = d.err[5] ? 'status-dot error' : 'status-dot';
      })
      .catch(e => console.error(e));
  }
  setInterval(updateData, 1000);
  updateData();
</script>
</body>
</html>
)rawliteral";

void handleRoot() {
    server.send(200, "text/html", index_html);
}

void handleApiData() {
    String json = "{";
    json += "\"waterLevelRaw\":" + String(currentData.waterLevelRaw) + ",";
    json += "\"waterLevelPercent\":" + String(currentData.waterLevelPercent) + ",";
    json += "\"lightLux\":" + String(currentData.lightLux) + ",";
    json += "\"tdsPPM\":" + String(currentData.tdsPPM) + ",";
    json += "\"airTempC\":" + String(currentData.airTempC) + ",";
    json += "\"humidityPercent\":" + String(currentData.humidityPercent) + ",";
    json += "\"vpdKpa\":" + String(currentData.vpdKpa) + ",";
    json += "\"phValue\":" + String(currentData.phValue) + ",";
    json += "\"waterTempC\":" + String(currentData.waterTempC) + ",";
    json += "\"err\":[" 
          + String(currentData.sensorError[SENSOR_WATER_LEVEL]) + "," 
          + String(currentData.sensorError[SENSOR_LIGHT]) + "," 
          + String(currentData.sensorError[SENSOR_TDS]) + "," 
          + String(currentData.sensorError[SENSOR_DHT22]) + "," 
          + String(currentData.sensorError[SENSOR_PH]) + "," 
          + String(currentData.sensorError[SENSOR_WATER_TEMP]) + "]";
    json += "}";
    
    server.send(200, "application/json", json);
}

void webDiagnoseInit() {
    server.on("/", handleRoot);
    server.on("/api/data", handleApiData);
    server.begin();
    DBGLN("[WEB] Diagnostic server started on port 80");
}

void webDiagnoseLoop(const SensorData& data) {
    currentData = data;
    server.handleClient();
}

#else

// Dummy implementations if disabled
void webDiagnoseInit() {}
void webDiagnoseLoop(const SensorData& data) {}

#endif // ENABLE_WEB_DIAGNOSE
