#include <firmwareupdater.h>
#include <WiFi.h>

const char* ssid = "iot";
const char* password = "IoT244466666";

firmwareupdater firmUP("https://updater.plug.farm/","0.0.0");

void setup() {
  Serial.begin(115200);
  WiFi.begin(ssid, password);
  firmUP.begin(10000);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected");
}

void loop() {
  firmUP.loop();
  DEBUG_PRINT("Hellou");
  DEBUG_PRINTLN("Hello World");
  delay(1000);
}

