#include <firmwareupdater.h>
#include <WiFi.h>

const char* ssid = "REPLACE_WITH_YOUR_SSID";
const char* password = "REPLACE_WITH_YOUR_PASSWORD";

firmwareupdater firmUP("https://linkforupdates.com/","0.0.0");

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

