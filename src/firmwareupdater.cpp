#include "firmwareupdater.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>

const char* _check;
const char* _upload;
const char* _confirm;
unsigned long _timing;
String chipIdString;

void initChipId(){
  uint64_t chipid = ESP.getEfuseMac();
  char chipidStr[20];
  sprintf(chipidStr, "%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
  chipIdString = String(chipidStr);
  
}

firmwareupdater::firmwareupdater(const char* check,const char* upload,const char* confirm){
    _check = check;
    _confirm = confirm;
    _upload = upload;
}


String checkForUpdate() {
  Serial.println("Checando Atualização");
  HTTPClient http;
  String CheckUpdate = _check + chipIdString;
  http.begin(CheckUpdate);
  int httpCode = http.GET();
  Serial.println(CheckUpdate);
  if (httpCode == 200) { // Sucesso
    Serial.println("Ping Realizado");
    String payload = http.getString();
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, payload);
    const char* status = doc["status"];
    const char* updateId = doc["atualizar"];
    Serial.print("Status: ");
    Serial.println(status);
    Serial.print("UpdateId: ");
    Serial.println(updateId);
    if (String(status) == "ok" && updateId != nullptr) {
      Serial.printf("Atualização disponível com ID: %s\n", updateId);
      http.end();
      return String(updateId); // Retorna o ID da atualização
    }
  }
  Serial.println("Check Finalizado");
  http.end();
  return ""; // Retorna vazio se não houver atualização
}

bool performUpdate(String firmwareDownloadUrl) {
  HTTPClient http;
  http.begin(firmwareDownloadUrl);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) { // Sucesso no download
    int contentLength = http.getSize();
    WiFiClient *client = http.getStreamPtr();

    if (contentLength > 0) {
      // Inicia a atualização
      bool canBegin = Update.begin(contentLength);
      
      if (canBegin) {
        size_t written = Update.writeStream(*client);
        
        if (written == contentLength) {
          Serial.println("Firmware atualizado com sucesso.");
          
          // Finaliza a atualização
          if (Update.end()) {
            return true; // Indica que a atualização foi bem-sucedida
          } else {
            Serial.println("Erro ao finalizar a atualização:");
            Update.printError(Serial);
          }
        } else {
          Serial.println("Ocorreu um erro ao escrever o firmware. Bytes escritos diferentes do esperado.");
          Update.printError(Serial);
        }
      } else {
        Serial.println("Falha ao iniciar a atualização");
        Update.printError(Serial);
      }
    } else {
      Serial.println("Erro: Tamanho do conteúdo inválido.");
    }
  } else {
    Serial.printf("Erro ao baixar o firmware. Código HTTP: %d\n", httpCode);
  }

  http.end();
  return false;
}

void confirmUpdate(String updateId) {
  HTTPClient http;
  String confirmURL = _confirm + updateId;
  Serial.println(confirmURL);
  http.begin(confirmURL);
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument doc(200);
  doc["status"] = "updated";
  doc["id"] = updateId;

  String jsonData;
  serializeJson(doc, jsonData);

  int httpCode = http.POST(jsonData);

  if (httpCode == 200) {
    Serial.println("Confirmação de atualização enviada com sucesso.");
  } else {
    Serial.println("Falha ao enviar confirmação de atualização.");
  }

  http.end();
}


void firmwareupdater::begin(unsigned long timing){
    _timing = timing;
    initChipId();
}



void firmwareupdater::loop(){
  static unsigned long lastCheck = 0;

  if (millis() - lastCheck > _timing) {
    lastCheck = millis();

    Serial.println("Verificando se há atualização disponível...");
    String updateId = checkForUpdate();

    if (updateId != "") { // Se recebeu um ID, faz o download e atualiza
      Serial.println("Nova atualização detectada. Iniciando download...");
      String firmwareURL= _upload + updateId;
      Serial.println(firmwareURL);
      if (performUpdate(firmwareURL)) {
        Serial.println("Reiniciando após atualização...");
        confirmUpdate(updateId); // Envia confirmação após reiniciar
        ESP.restart();
      }
    } else {
      Serial.println("Nenhuma atualização disponível.");
    }
  }
  
}
