#pragma once
// Brian's own setup hotspot.
//
// On first boot (no Wi-Fi known) or from Apparaatstatus > Details > "Wifi
// instellen", Brian opens a WPA2 hotspot "Brian-XXXX" with a fresh eight-digit
// password and shows both as a QR code. A phone that joins is sent to the
// setup page by the captive-portal redirect (DNS answers every name with
// Brian's own address). On that page networks can be added and removed; they
// are written to NVS immediately. "Opslaan en herstarten" restarts Brian, which
// then connects and enrols with the server.
//
// The page is plain server-rendered HTML with a tiny script for the scan list.
// No external resources: the phone has no internet while it is on this hotspot.

#include <DNSServer.h>
#include <WebServer.h>
#include "fleet_store.h"

static WebServer* vsPortalWeb = nullptr;
static DNSServer* vsPortalDns = nullptr;
static bool vsPortalActive = false;
static String vsPortalSsid;
static String vsPortalPass;
static uint32_t vsPortalRestartAt = 0;
static bool vsPortalChanged = false;
static int vsPortalLastClients = -1;
static uint8_t vsPortalLastNets = 255;
static const IPAddress VS_PORTAL_IP(192, 168, 4, 1);

static String vsHtmlEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else out += c;
  }
  return out;
}

static String vsJsonEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
    else out += c;
  }
  return out;
}

static const char VS_PORTAL_CSS[] PROGMEM = R"CSS(
*{box-sizing:border-box}body{margin:0;font:16px/1.45 -apple-system,Segoe UI,Roboto,sans-serif;
background:#f5f6f3;color:#20211f}main{max-width:520px;margin:0 auto;padding:18px 16px 40px}
h1{font-size:22px;margin:6px 0 2px}h2{font-size:16px;margin:22px 0 8px}
.sub{color:#62665f;font-size:14px;margin:0 0 14px}.card{background:#fff;border:1px solid #e4e7e2;
border-radius:12px;padding:14px;margin-bottom:12px}.net{display:flex;justify-content:space-between;
align-items:center;gap:10px;padding:8px 0;border-bottom:1px solid #eef0ec}.net:last-child{border:0}
label{display:block;font-size:13px;color:#62665f;margin:10px 0 4px;font-weight:600}
input,select{width:100%;font:inherit;padding:11px 12px;border:1px solid #cfd3cc;border-radius:9px;background:#fff}
button{font:inherit;font-weight:600;padding:11px 16px;border-radius:9px;border:1px solid #cfd3cc;background:#fff}
.primary{background:#6654d9;border-color:#6654d9;color:#fff;width:100%;margin-top:14px}
.danger{color:#b42332;border-color:#e2b5ba;padding:7px 12px;font-size:14px}
.muted{color:#62665f;font-size:13px}.ok{color:#246746}.row{display:flex;gap:8px}
.row>*{flex:1}.small{font-size:13px;padding:7px 10px}
)CSS";

static void vsPortalSendPage(const String& notice = String()) {
  String html;
  html.reserve(4096);
  html += F("<!doctype html><html lang=nl><head><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>Brian instellen</title><style>");
  html += FPSTR(VS_PORTAL_CSS);
  html += F("</style></head><body><main><h1>Brian instellen</h1><p class=sub>Voeg de "
            "wifi-netwerken toe waarmee Brian zijn opnames mag versturen. Maximaal ");
  html += String(VS_FLEET_MAX_NETWORKS);
  html += F(" netwerken.</p>");
  if (notice.length()) {
    html += F("<div class=card><b class=ok>");
    html += vsHtmlEscape(notice);
    html += F("</b></div>");
  }
  html += F("<div class=card><h2 style='margin-top:0'>Bekende netwerken</h2>");
  if (!vsFleetNetCount) {
    html += F("<p class=muted>Nog geen netwerken.</p>");
  }
  for (uint8_t i = 0; i < vsFleetNetCount; ++i) {
    html += F("<div class=net><span>");
    html += vsHtmlEscape(vsFleetNets[i].ssid);
    if (!vsFleetNets[i].pass.length()) html += F(" <span class=muted>(open)</span>");
    html += F("</span><form method=post action=/remove style='margin:0'>"
              "<input type=hidden name=ssid value=\"");
    html += vsHtmlEscape(vsFleetNets[i].ssid);
    html += F("\"><button class=danger>Verwijderen</button></form></div>");
  }
  html += F("</div><div class=card><h2 style='margin-top:0'>Netwerk toevoegen</h2>"
            "<form method=post action=/add autocomplete=off>"
            "<label for=pick>Netwerken in de buurt</label><div class=row>"
            "<select id=pick onchange=\"document.getElementById('ssid').value=this.value\">"
            "<option value=''>Zoeken...</option></select>"
            "<button type=button class=small style='flex:0 0 auto' onclick=scan(1)>Opnieuw</button></div>"
            "<label for=ssid>Netwerknaam (SSID)</label>"
            "<input id=ssid name=ssid maxlength=32 required autocapitalize=off autocorrect=off spellcheck=false>"
            "<label for=pass>Wachtwoord</label>"
            "<input id=pass name=pass type=password maxlength=63 autocomplete=new-password "
            "autocapitalize=off autocorrect=off spellcheck=false>"
            "<label style='font-weight:400;display:flex;gap:8px;align-items:center'>"
            "<input type=checkbox style='width:auto' onchange=\"document.getElementById('pass')"
            ".type=this.checked?'text':'password'\"> Wachtwoord tonen</label>"
            "<button class=primary>Toevoegen</button></form></div>"
            "<form method=post action=/finish><button class=primary>Opslaan en herstarten"
            "</button></form><p class=muted style='margin-top:14px'>Apparaat: ");
  html += vsHtmlEscape(String(vsFleetDeviceId()));
  html += F("<br>Na het herstarten maakt Brian verbinding met de server en toont hij een "
            "koppelcode voor de beheerder.</p></main><script>"
            "async function scan(again){const s=document.getElementById('pick');"
            "s.innerHTML='<option value=\"\">Zoeken...</option>';"
            "try{const r=await fetch(again?'/scan?refresh=1':'/scan');const l=await r.json();"
            "s.innerHTML='<option value=\"\">Kies een netwerk</option>';"
            "for(const n of l){const o=document.createElement('option');o.value=n.ssid;"
            "o.textContent=n.ssid+(n.secure?'':' (open)')+'  '+n.rssi+' dBm';s.appendChild(o);}"
            "if(!l.length)s.innerHTML='<option value=\"\">Niets gevonden</option>';"
            "}catch(e){s.innerHTML='<option value=\"\">Zoeken mislukt</option>';}}scan();"
            "</script></body></html>");
  vsPortalWeb->sendHeader("Cache-Control", "no-store");
  vsPortalWeb->send(200, "text/html; charset=utf-8", html);
}

static void vsPortalRedirectHome() {
  vsPortalWeb->sendHeader("Location", String("http://") + VS_PORTAL_IP.toString() + "/", true);
  vsPortalWeb->send(302, "text/plain", "Brian instellen: http://192.168.4.1/");
}

// Scanning while the hotspot is up makes the radio leave the hotspot's channel
// and phones may drop the page. So scan once before the hotspot starts and
// serve that list; "Opnieuw" rescans on explicit request only.
static String vsPortalScanJson = "[]";

static void vsPortalScan() {
  const int found = WiFi.scanNetworks(false, false);
  std::vector<int> picked;   // deduplicate by SSID (mesh systems), keep the strongest
  for (int i = 0; i < found; ++i) {
    const String s = WiFi.SSID(i);
    if (!s.length()) continue;
    bool dup = false;
    for (int j : picked) {
      if (WiFi.SSID(j) == s) {
        dup = true;
        break;
      }
    }
    if (!dup) picked.push_back(i);
  }
  std::sort(picked.begin(), picked.end(),
            [](int a, int b) { return WiFi.RSSI(a) > WiFi.RSSI(b); });
  String json = "[";
  for (size_t k = 0; k < picked.size() && k < 25; ++k) {
    const int i = picked[k];
    if (k) json += ",";
    json += "{\"ssid\":\"" + vsJsonEscape(WiFi.SSID(i)) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
            ",\"secure\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true") + "}";
  }
  json += "]";
  WiFi.scanDelete();
  vsPortalScanJson = json;
  Serial.printf("PORTAL: scan found %d network(s)\n", (int)picked.size());
}

static void vsPortalHandleScan() {
  if (vsPortalWeb->hasArg("refresh")) vsPortalScan();
  vsPortalWeb->sendHeader("Cache-Control", "no-store");
  vsPortalWeb->send(200, "application/json", vsPortalScanJson);
}

static void vsPortalHandleAdd() {
  const String ssid = vsPortalWeb->arg("ssid");
  const String pass = vsPortalWeb->arg("pass");
  if (!ssid.length() || ssid.length() > 32) {
    vsPortalSendPage("Vul een netwerknaam in (maximaal 32 tekens).");
    return;
  }
  if (pass.length() && (pass.length() < 8 || pass.length() > 63)) {
    vsPortalSendPage("Een wifi-wachtwoord is 8 tot 63 tekens, of leeg voor een open netwerk.");
    return;
  }
  if (vsFleetFindNetwork(ssid) < 0 && vsFleetNetCount >= VS_FLEET_MAX_NETWORKS) {
    vsPortalSendPage("De lijst is vol; verwijder eerst een netwerk.");
    return;
  }
  if (!vsFleetUpsertNetwork(ssid, pass)) {
    vsPortalSendPage("Opslaan mislukt.");
    return;
  }
  vsPortalChanged = true;
  Serial.printf("PORTAL: network saved ssid=%s\n", ssid.c_str());
  vsPortalSendPage(String("Opgeslagen: ") + ssid);
}

static void vsPortalHandleRemove() {
  const String ssid = vsPortalWeb->arg("ssid");
  vsFleetRemoveNetwork(ssid);
  vsPortalChanged = true;
  Serial.printf("PORTAL: network removed ssid=%s\n", ssid.c_str());
  vsPortalSendPage(String("Verwijderd: ") + ssid);
}

static void vsPortalHandleFinish() {
  if (!vsFleetNetCount) {
    // First-time setup is not done without at least one network.
    vsPortalSendPage("Voeg eerst minstens een wifi-netwerk toe.");
    return;
  }
  String html = F("<!doctype html><html lang=nl><head><meta charset=utf-8>"
                  "<meta name=viewport content='width=device-width,initial-scale=1'><style>");
  html += FPSTR(VS_PORTAL_CSS);
  html += F("</style></head><body><main><h1>Brian herstart</h1><p class=sub>");
  if (vsFleetNetCount) {
    html += F("Brian maakt nu verbinding met de server. Je kunt deze pagina sluiten en je "
              "telefoon weer met je eigen wifi verbinden.");
  } else {
    html += F("Er is geen netwerk ingesteld. Brian neemt gewoon op, maar kan niets versturen "
              "tot er een netwerk is.");
  }
  html += F("</p></main></body></html>");
  vsPortalWeb->send(200, "text/html; charset=utf-8", html);
  vsPortalRestartAt = millis() + 1500;
}

static void vsPortalStop() {
  if (vsPortalWeb) {
    vsPortalWeb->stop();
    delete vsPortalWeb;
    vsPortalWeb = nullptr;
  }
  if (vsPortalDns) {
    vsPortalDns->stop();
    delete vsPortalDns;
    vsPortalDns = nullptr;
  }
  WiFi.softAPdisconnect(true);
  vsPortalActive = false;
  vsPortalRestartAt = 0;
}

static bool vsPortalStart() {
  if (vsPortalActive) return true;
  String mac = vsFleetMacHex();
  mac.toUpperCase();
  vsPortalSsid = String("Brian-") + mac.substring(8);
  char pass[9];
  snprintf(pass, sizeof(pass), "%08lu", (unsigned long)(esp_random() % 100000000UL));
  vsPortalPass = pass;

  WiFi.disconnect(true, true);
  delay(50);
  WiFi.mode(WIFI_AP_STA);   // STA side is used only for scanning
  delay(100);
  vsPortalScan();
  WiFi.softAPConfig(VS_PORTAL_IP, VS_PORTAL_IP, IPAddress(255, 255, 255, 0));
  if (!WiFi.softAP(vsPortalSsid.c_str(), vsPortalPass.c_str(), 6, 0, 2)) {
    Serial.println("PORTAL: softAP start failed");
    WiFi.mode(WIFI_OFF);
    return false;
  }
  WiFi.setSleep(false);

  vsPortalDns = new DNSServer();
  vsPortalDns->setErrorReplyCode(DNSReplyCode::NoError);
  vsPortalDns->start(53, "*", VS_PORTAL_IP);

  vsPortalWeb = new WebServer(80);
  vsPortalWeb->on("/", HTTP_GET, []() { vsPortalSendPage(); });
  vsPortalWeb->on("/scan", HTTP_GET, vsPortalHandleScan);
  vsPortalWeb->on("/add", HTTP_POST, vsPortalHandleAdd);
  vsPortalWeb->on("/remove", HTTP_POST, vsPortalHandleRemove);
  vsPortalWeb->on("/finish", HTTP_POST, vsPortalHandleFinish);
  // Captive-portal probes of Android, iOS/macOS and Windows all land on the page.
  vsPortalWeb->onNotFound(vsPortalRedirectHome);
  vsPortalWeb->begin();

  vsPortalActive = true;
  vsPortalChanged = false;
  vsPortalLastClients = -1;
  vsPortalLastNets = 255;
  Serial.printf("PORTAL: hotspot %s up at %s\n", vsPortalSsid.c_str(),
                VS_PORTAL_IP.toString().c_str());
  return true;
}

// Returns true once the page asked for a restart and the reply had time to go out.
static bool vsPortalService() {
  if (!vsPortalActive) return false;
  vsPortalDns->processNextRequest();
  vsPortalWeb->handleClient();
  return vsPortalRestartAt && (int32_t)(millis() - vsPortalRestartAt) >= 0;
}

static String vsPortalQrText() {
  // Standard Wi-Fi QR payload understood by the iOS and Android cameras.
  return String("WIFI:T:WPA;S:") + vsPortalSsid + ";P:" + vsPortalPass + ";;";
}
