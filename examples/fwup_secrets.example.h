#pragma once

// Copy to fwup_secrets.h next to your project's main.cpp and fill in the real
// values. fwup_secrets.h is gitignored: deployment addresses and credentials
// never belong in the library.
//
//   cp examples/fwup_secrets.example.h src/fwup_secrets.h
//
// Note how short this list is. Everything about the broker - host, port, TLS,
// username, password and the three topic strings - arrives in the provisioning
// response and is persisted to NVS, so none of it is compiled in.

// Bootstrap API. This is the one address that genuinely has to be here:
// api_base_url comes back in the provisioning response, but the device needs
// somewhere to send that first request.
#define FWUP_API_BASE_URL       "https://updater.example.com"

// Plain-HTTP base for the cellular link, which cannot negotiate TLS.
// Same value as above when the deployment has a single host.
#define FWUP_API_BASE_URL_GPRS  "http://updater.example.com"

// Wi-Fi credentials, when the project does not provision them over BLE.
#define FWUP_WIFI_SSID          ""
#define FWUP_WIFI_PASS          ""

// Cellular APN.
#define FWUP_GPRS_APN           "apn.example.com"
#define FWUP_GPRS_USER          ""
#define FWUP_GPRS_PASS          ""
