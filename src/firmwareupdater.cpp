#include "firmwareupdater.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>

bool debugAtivo = false;
const char* _site;
const char* _upload;
const char* _confirm;
unsigned long _timing;
String chipIdString;
const char* CURRENT_VERSION;

unsigned long tempoPing = 300000; // valor padrão
String _updateId = "";

void initChipId(){
  uint64_t chipid = ESP.getEfuseMac();
  char chipidStr[20];
  sprintf(chipidStr, "%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
  chipIdString = String(chipidStr);
  
}

firmwareupdater::firmwareupdater(const char* site,const char* version){
    _site = site;
    CURRENT_VERSION = version;
}

String checkForUpdate() {
  Serial.println("Checando Atualização");

  HTTPClient http;
  String CheckUpdate = String(_site) + "atualizador/ping/" + chipIdString + "/" + CURRENT_VERSION;
  Serial.println(CheckUpdate);

  http.begin(CheckUpdate);
  int httpCode = http.GET();

  if (httpCode == 200) {
    Serial.println("Ping Realizado");
    String payload = http.getString();

    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      const char* status = doc["status"];
      debugAtivo = doc["debug"];
      _timing = doc["tempoPing"] | _timing;  // fallback para 5 min se não vier
      _updateId = doc["atualizar"] | "";

      Serial.print("Status: ");
      Serial.println(status);
      Serial.print("UpdateId: ");
      Serial.println(_updateId);
      Serial.print("Debug: ");
      Serial.println(debugAtivo ? "Ativo" : "Inativo");
      Serial.print("TempoPing: ");
      Serial.println(tempoPing);

      if (String(status) == "ok" && _updateId != "") {
        Serial.printf("Atualização disponível com ID: %s\n", _updateId.c_str());
        http.end();
        return _updateId;
      }
    } else {
      Serial.print("Erro ao parsear JSON: ");
      Serial.println(error.c_str());
    }
  }

  Serial.println("Check Finalizado");
  http.end();
  return "";
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
  String confirmURL = String(_site) + "atualizador/confirmarAtualizacao/" + updateId;
  Serial.println(confirmURL);
  http.begin(confirmURL);

  // Use GET em vez de POST
  int httpCode = http.GET();

  if (httpCode == 200) {
    Serial.println("Confirmação de atualização enviada com sucesso.");
  } else {
    Serial.println("Falha ao enviar confirmação de atualização.");
  }

  http.end();
}


void firmwareupdater::print(const String& msg, const char* arquivo, int linha, const char* funcao) {
  Serial.println(msg);

  if (debugAtivo) {
    HTTPClient http;
    http.begin(String(_site) + "debug-log");
    http.addHeader("Content-Type", "application/json");

    DynamicJsonDocument doc(512);
    doc["codigo"] = chipIdString;
    doc["mensagem"] = msg;
    doc["arquivo"] = arquivo;
    doc["linha"] = linha;
    doc["funcao"] = funcao;

    String requestBody;
    serializeJson(doc, requestBody);

    int httpCode = http.POST(requestBody);

    if (httpCode > 0) {
      Serial.printf("Debug enviado. Código HTTP: %d\n", httpCode);
    } else {
      Serial.printf("Erro ao enviar debug: %s\n", http.errorToString(httpCode).c_str());
    }

    http.end();
  }
}

void firmwareupdater::println(const String& msg, const char* arquivo, int linha, const char* funcao) {
  Serial.println(msg);

  if (debugAtivo) {
    HTTPClient http;
    http.begin(String(_site) + "debug-log");
    http.addHeader("Content-Type", "application/json");

    DynamicJsonDocument doc(512);
    doc["codigo"] = chipIdString;
    doc["mensagem"] = msg +"\n" ;
    doc["arquivo"] = arquivo;
    doc["linha"] = linha;
    doc["funcao"] = funcao;

    String requestBody;
    serializeJson(doc, requestBody);

    int httpCode = http.POST(requestBody);

    if (httpCode > 0) {
      Serial.printf("Debug enviado. Código HTTP: %d\n", httpCode);
    } else {
      Serial.printf("Erro ao enviar debug: %s\n", http.errorToString(httpCode).c_str());
    }

    http.end();
  }
}

void firmwareupdater::begin(unsigned long timing){
    _timing = timing;
    initChipId();
}



void firmwareupdater::loop(){
  static unsigned long lastCheck = 0;

  if (millis() - lastCheck > _timing) {
    lastCheck = millis();
    
    Serial.println("Verificando se há atualização disponível ou Ativação de Debug...");
    String updateId = checkForUpdate();

    if (updateId != "") { // Se recebeu um ID, faz o download e atualiza
      Serial.println("Nova atualização detectada. Iniciando download...");
      String firmwareURL= String(_site) + "atualizador/atualizar/" + updateId;
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
  else{
    return;
  }
  
}
