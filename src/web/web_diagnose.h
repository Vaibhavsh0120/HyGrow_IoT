#ifndef WEB_DIAGNOSE_H
#define WEB_DIAGNOSE_H

#include "../../config.h"
#include <WebServer.h>

void webDiagnoseInit();
void webDiagnoseLoop(const SensorData& data);

#endif // WEB_DIAGNOSE_H
