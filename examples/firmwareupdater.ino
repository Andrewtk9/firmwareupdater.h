#include <firmwareupdater.h>
#include <WiFi.h>

const char* ssid = "REPLACE_WITH_YOUR_SSID";
const char* password = "REPLACE_WITH_YOUR_PASSWORD";

firmwareupdater firmUP("https://linkforping.com/",
                  "https://linkfordownloadBinFile.com/",
                  "https://linkforConfirmUpdate.com/");

void setup() {
  Serial.begin(115200);
  WiFi.begin(ssid, password);
  firmUP.begin(4000);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConected");
}

void loop() {
  firmUP.loop();

}
