
# firmwareupdater

A lightweight library for **OTA firmware updates** and **remote debug logging** using WebService, built for ESP32 devices using the Arduino framework.

## 🚀 Features

- 🔄 **OTA firmware update** via HTTP
- 🐞 **Remote debug logging** via HTTP POST in JSON format
- 🆙 Device auto-checks for updates based on version
- 📡 Periodic polling to check if update or debug is enabled
- 🧠 Intelligent debug control (only sends logs if server returns `debug: true`)
- 📎 Automatic inclusion of file, line, and function in logs

---

## 🔧 How It Works

The library does two main things:

1. **Check for updates**:

   - Periodically pings an endpoint:`https://yourserver.com/atualizador/ping/<device_id>/<current_version>`
   - If `{"status": "ok", "atualizar": "<updateId>"}` is received, it downloads and installs the update.
   - It also receives:
     - `debug`: enables/disables remote debug log sending.
     - `tempoPing`: interval (in ms) between pings.
2. **Send logs remotely**:

   - All logs use `Serial.println()` locally.
   - If `debug` is `true`, it sends JSON log data to:`https://yourserver.com/debug-log`
   - Example JSON:
     ```json
     {
       "codigo": "F8E89CA7DBCC",
       "mensagem": "Failed to connect",
       "arquivo": "main.cpp",
       "linha": 42,
       "funcao": "setup"
     }
     ```

---

## 📦 Usage

### 1. Include and configure

```cpp
#include "firmwareupdater.h"

firmwareupdater firmUP("https://yourserver.com/", "1.0.0");
2. Setup
cpp
Copiar
Editar
void setup() {
  Serial.begin(115200);
  WiFi.begin("your_ssid", "your_password");
  while (WiFi.status() != WL_CONNECTED) delay(500);

  firmUP.begin(30000);  // Check every 30 seconds
}
3. Loop
cpp
Copiar
Editar
void loop() {
  firmUP.loop();  // Must be called frequently

  DEBUG_PRINTLN("Loop running...");  // Sends log only if debug mode is active
  delay(5000);
}
4. Logging macro (optional)
To include file, line, and function automatically, use this macro:

cpp
Copiar
Editar
#define DEBUG_PRINTLN(msg) firmUP.println(msg, __FILE__, __LINE__, __FUNCTION__)
📡 Server Endpoints (Expected)
GET /atualizador/ping/<chipId>/<version>
Returns JSON:

json
Copiar
Editar
{
  "debug": true,
  "atualizar": "update123",  // or null
  "tempoPing": 300000,
  "status": "ok"
}
GET /atualizador/atualizar/<updateId>
Returns binary firmware for OTA update.

GET /atualizador/confirmarAtualizacao/<updateId>
Confirms that update was applied successfully.

POST /debug-log
Receives log entries in JSON format.
```

Lembre -se de sempre confefir os Cintos d =e segurança
