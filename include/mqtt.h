#pragma once
#include <Arduino.h>

void setupMQTT();
void handleMQTT();
void publishStatus();

// Set by /update when the water sensor is enabled/disabled; handleMQTT()
// then adds or removes the Home Assistant "Low water" entity.
extern volatile bool mqttWaterDiscoveryDirty;
