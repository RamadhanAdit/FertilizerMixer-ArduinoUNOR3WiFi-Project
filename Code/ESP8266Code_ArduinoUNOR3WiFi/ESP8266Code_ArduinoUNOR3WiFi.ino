/* ------ ESP8266 CODE (NodeMCU/ESP-01 style) ------ 
   Virtual pins:
   V0 - (optional) LCD or status
   V1 - setpoint ppm (Write)
   V2 - pH real display (Write from ESP to Blynk)
   V3 - ppm real display (Write from ESP to Blynk)
   V4 - Valve4 button (Switch) (Write from Blynk -> ESP)
   V5 - setpoint pH (Write)
   V6 - ON duration seconds (Write -> send to ATmega)
   V7 - OFF duration minutes (Write -> send to ATmega)
   V8 - Valve 1 Status
   V9 - Valve 2 Status
   V10 - Valve 3 Status
   V11 - Valve 4 Status
*/

//#define BLYNK_PRINT Serial

/*
// ------ TESTING ------
#define BLYNK_TEMPLATE_ID "TMPL6J-QXIrRR"
#define BLYNK_TEMPLATE_NAME "Pencampur Pupuk"
#define BLYNK_AUTH_TOKEN "9ZRRYnPTGlAfW0msnCDtSlfz99t227kM"
*/

#define BLYNK_TEMPLATE_ID "TMPL6zUzwkmOK"
#define BLYNK_TEMPLATE_NAME "Pencampur Pupuk"
#define BLYNK_AUTH_TOKEN "8uSwICzmndEhEjwKhBs4QBZrGbAEsJf8"

#include <ESP8266WiFi.h>
#include <BlynkSimpleEsp8266.h>
#include <WiFiManager.h>

BlynkTimer timer;
int sendNetStatusCount;
bool wifiStatus = false;

unsigned long lastWifiCheck = 0;
int wifiFailCount = 0;
int blynkFailCount = 0;
bool onceTimeSendBlynk = false;
bool onceTimeSendWifi = false;
const int WIFI_FAIL_REBOOT = 10;
const int BLYNK_FAIL_RECONNECTS = 6;

// ------ Send to Atemega ------
void sendToATmega(String s) {
  s.trim();
  if (s.length() == 0) return;
  Serial.println(s);  // serial0 to ATmega (cross-connected)
}

// ------ Network status parsing ------
void sendNetStatus() {
  bool wifiOK = (WiFi.status() == WL_CONNECTED);
  bool blynkOK = Blynk.connected();
  if (sendNetStatusCount < 5) {
    String ipStr = (wifiOK) ? WiFi.localIP().toString() : String("0.0.0.0");
    String msg = "NET WIFI:" + String(wifiOK ? 1 : 0) + " BLYNK:" + String(blynkOK ? 1 : 0) + " IP:" + ipStr;
    sendToATmega(msg);
    sendNetStatusCount ++;
  } else if ((!blynkOK && wifiOK) && !wifiStatus) {
    sendNetStatusCount = 0;
    wifiStatus = true;
  } else if (!blynkOK && !wifiOK) {
    sendNetStatusCount = 0;
    wifiStatus = false;
  }
}

// ------ Network connection check ------
void checkConnections(){
  bool wifiOK = (WiFi.status() == WL_CONNECTED);
  bool blynkOK = Blynk.connected();

  // WiFi handling
  if (!wifiOK && !onceTimeSendWifi){
    sendNetStatus();
    delay(100);
    onceTimeSendWifi = true;
  } else if (wifiOK) {
    onceTimeSendWifi = false;
    wifiFailCount = 0;
  }

  // Blynk handling
  if ((!blynkOK && wifiOK) && !onceTimeSendBlynk){
    sendNetStatus();
    delay(100);
    onceTimeSendBlynk = true;
  } else if (blynkOK){
    blynkFailCount = 0;
    onceTimeSendBlynk = false;
  }

  // If WiFi & Blynk has been failing many times -> reboot to recover
  if (wifiFailCount >= WIFI_FAIL_REBOOT || blynkFailCount >= BLYNK_FAIL_RECONNECTS){
    sendNetStatus();
    delay(100);
    ESP.restart();
  }

  // Always send periodic NET status (adaptive)
  static unsigned long lastNetSent = 0;
  unsigned long now = millis();
  unsigned long interval = blynkOK ? 3000UL : 15000UL; // fast when connected, slow when offline
  if (now - lastNetSent >= interval){
    lastNetSent = now;
    sendNetStatus();
    if (!blynkOK && wifiOK) {
      blynkFailCount++;
    } else if (!wifiOK){
      wifiFailCount++;
      WiFi.reconnect();
    }
  }
}

void setup() {
  Serial.begin(9600);  // for serial communication to Atmega
  delay(100);
  WiFiManager wm;
  wm.autoConnect("Fertilizer Mixer");
  Blynk.config(BLYNK_AUTH_TOKEN);
  Blynk.connect();
  Serial.println("Connected to Blynk!");
  timer.setInterval(5000L, requestSensorData); 
  timer.setInterval(1000L, checkConnections);
}

void loop() {
  Blynk.run();
  timer.run();
  readFromATmega();
}

void requestSensorData() {
  if (Blynk.connected()){
    sendToATmega("DATREQ");
  }
}

// Read incoming data from ATmega
String lineBuf = "";
void readFromATmega() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      lineBuf.trim();
      // Parse data
      if (lineBuf.startsWith("DATA")) {
        float pH = 0.0;
        int ppm = 0;
        int idx = lineBuf.indexOf("pH:");
        if (idx >= 0) {
          int idx2 = lineBuf.indexOf(' ', idx);
          String s;
          if (idx2 > idx) s = lineBuf.substring(idx + 3, idx2);
          else s = lineBuf.substring(idx + 3);
          pH = s.toFloat();
        }
        idx = lineBuf.indexOf("ppm:");
        if (idx >= 0) {
          int idx2 = lineBuf.indexOf(' ', idx);
          String s;
          if (idx2 > idx) s = lineBuf.substring(idx + 4, idx2);
          else s = lineBuf.substring(idx + 4);
          ppm = s.toInt();
        } 
        // parse valve statuses (v1..v4) if present
        int v1 = -1, v2 = -1, v3 = -1, v4 = -1;
        idx = lineBuf.indexOf("v1:");
        if (idx >= 0) {
          int idx2 = lineBuf.indexOf(' ', idx);
          String s = (idx2 > idx) ? lineBuf.substring(idx + 3, idx2) : lineBuf.substring(idx + 3);
          v1 = s.toInt();
        }
        idx = lineBuf.indexOf("v2:");
        if (idx >= 0) {
          int idx2 = lineBuf.indexOf(' ', idx);
          String s = (idx2 > idx) ? lineBuf.substring(idx + 3, idx2) : lineBuf.substring(idx + 3);
          v2 = s.toInt();
        }
        idx = lineBuf.indexOf("v3:");
        if (idx >= 0) {
          int idx2 = lineBuf.indexOf(' ', idx);
          String s = (idx2 > idx) ? lineBuf.substring(idx + 3, idx2) : lineBuf.substring(idx + 3);
          v3 = s.toInt();
        }
        idx = lineBuf.indexOf("v4:");
        if (idx >= 0) {
          int idx2 = lineBuf.indexOf(' ', idx);
          String s = (idx2 > idx) ? lineBuf.substring(idx + 3, idx2) : lineBuf.substring(idx + 3);
          v4 = s.toInt();
        }
        // send to Blynk
        Blynk.virtualWrite(V2, String(pH, 2));
        Blynk.virtualWrite(V3, ppm);
        if (v1 >= 0) Blynk.virtualWrite(V8, v1);
        if (v2 >= 0) Blynk.virtualWrite(V9, v2);
        if (v3 >= 0) Blynk.virtualWrite(V10, v3);
        if (v4 >= 0) Blynk.virtualWrite(V11, v4);

      }
      // SETPH parsing (from UNO keypad)
      else if (lineBuf.startsWith("SETPH:")) {
        // format: SETPH:7.20
        String v = lineBuf.substring(6);
        v.trim();
        float val = v.toFloat();
        // update Blynk setpoint widget
        Blynk.virtualWrite(V5, String(val, 2));  // V5 = setpoint pH
      }
      // SETPPM parsing (from UNO keypad)
      else if (lineBuf.startsWith("SETPPM:")) {
        // format: SETPPM:500
        String v = lineBuf.substring(7);
        v.trim();
        int val = v.toInt();
        // update Blynk setpoint widget
        Blynk.virtualWrite(V1, val);  // V1 = setpoint PPM
      }
      // SETONDUR parsing (from UNO keypad)
      else if (lineBuf.startsWith("SETONDUR:")) {
        String v = lineBuf.substring(9);
        v.trim();
        int val = v.toInt();
        // update Blynk duration widget (V6 = ON duration seconds)
        Blynk.virtualWrite(V6, val);
      }
      // SETOFFDUR parsing (from UNO keypad)
      else if (lineBuf.startsWith("SETOFFDUR:")) {
        String v = lineBuf.substring(10);
        v.trim();
        int val = v.toInt();
        // update Blynk duration widget (V7 = OFF duration minute)
        Blynk.virtualWrite(V7, val);
      }
      lineBuf = "";
    } else if (c != '\r') {
      lineBuf += c;
    }
  }
}

// ------ If Blynk connected ------
BLYNK_CONNECTED() {
  sendToATmega("SETREQ");   // Setpoint request
  sendToATmega("DATREQ");   // Data request
  sendNetStatus();          // Send network status
  sendNetStatusCount = 0;   // Set sendNetStatusCount to 0
  wifiStatus = false;       // Set wifiStatus to false
}

// ------ Blynk handler
BLYNK_WRITE(V5) {  // setpoint pH input from app
  float newPH = param.asFloat();
  if (newPH >= 0.0 && newPH <= 14.0) {
    String cmd = "SETPH:" + String(newPH, 2);
    sendToATmega(cmd);
  }
}

BLYNK_WRITE(V1) {  // setpoint ppm from app
  int newPPM = param.asInt();
  if (newPPM >= 0 && newPPM <= 2000) {
    String cmd = "SETPPM:" + String(newPPM);
    sendToATmega(cmd);
  }
}

BLYNK_WRITE(V4) {  // Valve4 control switch (1=ON,0=OFF) from app
  int v = param.asInt();
  if (v == 1) {
    sendToATmega("VALVE4 ON");
  } else {
    sendToATmega("VALVE4 OFF");
  }
}

BLYNK_WRITE(V6) {  // ON duration seconds changed from app
  int newOn = param.asInt();
  if (newOn >= 1 && newOn <= 3600) {
    String cmd = "SETONDUR:" + String(newOn);
    sendToATmega(cmd);
  }
}

BLYNK_WRITE(V7) {  // OFF duration minutes changed from app
  int newOff = param.asInt();
  if (newOff >= 0 && newOff <= 10000) {
    String cmd = "SETOFFDUR:" + String(newOff);
    sendToATmega(cmd);
  }
}