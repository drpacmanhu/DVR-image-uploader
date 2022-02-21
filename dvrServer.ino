#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include "FS.h"
#include <WiFiClientSecure.h>

const String DVR_ADDRESS = "http://<DVR_SERVER_LOCAL_ADDRESS>";
const String DVR_SERVER_URL = "/ISAPI/Streaming/channels/{cameraNumber}/picture";
const String DVR_SERVER_PORT = "80";
const String DVR_SERVER_USER = "DVR_SERVER_USER";
const String DVR_SERVER_PASSWORD = "DVR_SERVER_USER_PASSWORD";
const int POOLING_INTERVAL = 30000;

const char* CLOUD_ADDRESS = "CLOUD_SERVER_ADDRESS";
const int CLOUD_PORT = 443;
const String CLOUD_URL = "/dvrServer/communication/uploadContent.php";

const String WIFI_NAME     = "WIFI_SSID";
const String WIFI_PASSWORD = "WIFI_PASSWORD!";
const byte RED_LED = D7;
const byte GREEN_LED = D6;
bool activityLed = false;
int wifiSignalStrength = 0;

//Set led states
void setLedStates() {
  if (activityLed) {
    digitalWrite(GREEN_LED, HIGH);
    activityLed = false;
  } else {
    digitalWrite(GREEN_LED, LOW);
    activityLed  = true;
  }
  //refresh the wifi signal
  if (wifiSignalStrength >= -75) {
    //fair
    digitalWrite(RED_LED, LOW);
  } else {
    //weak signal indicator
    digitalWrite(RED_LED, HIGH);
  }
}

void connectToWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_NAME, WIFI_PASSWORD);
  int connectionLoop = 0;
  Serial.println("Trying to connect to wifi...");
  //if we cannot connect to wifi within watchdog period restart the board
  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(RED_LED, LOW);
    delay(250);
    Serial.println("Connecting to WIFI...");
    connectionLoop ++;
    if (connectionLoop > 30) {
      ESP.restart();
    }
    digitalWrite(RED_LED, HIGH);
    delay(250);
  }
  setLedStates();
  wifiSignalStrength = WiFi.RSSI();
}

void setup() {
  Serial.begin(115200);
  Serial.println("Setup started...");
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);

  Serial.println(F("Inizializing FS..."));
  if (SPIFFS.begin()) {
    Serial.println(F("done."));
  } else {
    Serial.println(F("fail."));
  }
  //start wifi connection
  connectToWifi();

  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}

String exractParam(String& authReq, const String& param, const char delimit) {
  int _begin = authReq.indexOf(param);
  if (_begin == -1) {
    return "";
  }
  return authReq.substring(_begin + param.length(), authReq.indexOf(delimit, _begin + param.length()));
}

String getCNonce(const int len) {
  static const char alphanum[] =
    "0123456789"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz";
  String s = "";

  for (int i = 0; i < len; ++i) {
    s += alphanum[rand() % (sizeof(alphanum) - 1)];
  }

  return s;
}

String getDigestAuth(String& authReq, const String& username, const String& password, const String& method, const String& uri, unsigned int counter) {
  // extracting required parameters for RFC 2069 simpler Digest
  String realm = exractParam(authReq, "realm=\"", '"');
  String nonce = exractParam(authReq, "nonce=\"", '"');
  String cNonce = getCNonce(8);

  char nc[9];
  snprintf(nc, sizeof(nc), "%08x", counter);

  // parameters for the RFC 2617 newer Digest
  MD5Builder md5;
  md5.begin();
  md5.add(username + ":" + realm + ":" + password);  // md5 of the user:realm:user
  md5.calculate();
  String h1 = md5.toString();

  md5.begin();
  md5.add(method + ":" + uri);
  md5.calculate();
  String h2 = md5.toString();

  md5.begin();
  md5.add(h1 + ":" + nonce + ":" + String(nc) + ":" + cNonce + ":" + "auth" + ":" + h2);
  md5.calculate();
  String response = md5.toString();

  String authorization = "Digest username=\"" + username + "\", realm=\"" + realm + "\", nonce=\"" + nonce +
                         "\", uri=\"" + uri + "\", algorithm=\"MD5\", qop=auth, nc=" + String(nc) + ", cnonce=\"" + cNonce + "\", response=\"" + response + "\"";
  //Serial.println(authorization);

  return authorization;
}

String getFileName(int paramCameraNumber) {
  return String("/data/cameraImage-" + String(paramCameraNumber * 100) + ".png");
}

void getCameraImage(int paramCameraNumber) {
  String cameraNumber = String(paramCameraNumber * 100);
  String server = DVR_ADDRESS;
  String tmpUri = "/ISAPI/Streaming/channels/" + cameraNumber + "/picture";
  WiFiClient client;
  HTTPClient http; //must be declared after WiFiClient for correct destruction order, because used by http.begin(client,...)
  String tmpFilename = getFileName(paramCameraNumber);
  SPIFFS.remove(tmpFilename);
  setLedStates();
  Serial.println("[HTTP] begin...");
  http.begin(client, server + tmpUri);

  const char *keys[] = {"WWW-Authenticate"};
  http.collectHeaders(keys, 1);

  Serial.println("[HTTP] GET...");
  // start connection and send HTTP header
  int httpCode = http.GET();
  setLedStates();
  if (httpCode > 0) {
    String authReq = http.header("WWW-Authenticate");
    String authorization = getDigestAuth(authReq, DVR_SERVER_USER, DVR_SERVER_PASSWORD, "GET", tmpUri, 1);
    http.end();
    http.begin(client, server + tmpUri);
    http.addHeader("Authorization", authorization);
    int httpCode = http.GET();
    File actualCameraFile = SPIFFS.open(tmpFilename, "a+");
    setLedStates();
    if (httpCode > 0) {
      if (httpCode == HTTP_CODE_OK) {
        // get length of document (is -1 when Server sends no Content-Length header)
        int len = http.getSize();
        Serial.print("getSize : ");
        Serial.println(len);
        http.writeToStream(&actualCameraFile);
        actualCameraFile.close();
        Serial.print("[HTTP] connection closed or file end.\n");
      } else {
        Serial.println("HTTP RESP CODE WAS NOT OK");
      }
    } else {
      Serial.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
    }
  } else {
    Serial.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
  setLedStates();
}

void uploadCameraImage(int paramCameraNumber) {
  String Link = CLOUD_URL;
  const int httpsPort = CLOUD_PORT;
  //Declare object of class WiFiClient
  //WiFiClient client;
  WiFiClientSecure httpsClient;
  setLedStates();

  httpsClient.setInsecure();
  httpsClient.setTimeout(15000); // 15 Seconds
  delay(1000);
  setLedStates();

  //Serial.println("Establishing connection to : " + CLOUD_ADDRESS + ":" + CLOUD_PORT);
  Serial.println("Establishing connection to " + String(CLOUD_ADDRESS) + " : " + String(CLOUD_PORT) + "...");
  int r = 0; //retry counter
  while ((!httpsClient.connect(CLOUD_ADDRESS, CLOUD_PORT)) && (r < 30)) {
    Serial.print(".");
    r++;
    ESP.wdtFeed();
    delay(100);
  }
  if (r == 30) {
    Serial.println("Connection failed");
    return;
  }
  else {
    Serial.println("Connected...");
  }
  String tmpFilename = getFileName(paramCameraNumber);
  File cameraFile = SPIFFS.open(tmpFilename, "r");
  setLedStates();
  Serial.println("File length for sending was: " + String(cameraFile.size()));
  httpsClient.println("PUT " + String(CLOUD_URL) + " HTTP/1.1");
  httpsClient.println("Host: " + String(CLOUD_ADDRESS));
  httpsClient.println("Content-Type: image/png");
  httpsClient.println("Content-Length: " + String(cameraFile.size()));
  httpsClient.println("cameraNumber: " + String(paramCameraNumber));
  httpsClient.println(""); //adding extra empty line before body
  httpsClient.write(cameraFile);
  cameraFile.close();
  Serial.println("waiting for reply from server...");
  setLedStates();
  while (httpsClient.connected()) {
    String line = httpsClient.readStringUntil('\n');
    if (line == "\r") {
      Serial.println("headers received from server...");
      break;
    }
    Serial.print(".");
    ESP.wdtFeed();
    delay(100);
    setLedStates();
  }

  Serial.print("reply from server was: ");
  String line;
  while (httpsClient.available()) {
    line = httpsClient.readStringUntil('\n');  //Read Line by Line
    Serial.println(line); //Print response
  }
  Serial.println("closing connection");
  httpsClient.stop();
  setLedStates();
}

void loop() {
  for (int i = 1; i < 5; i++) {
    Serial.println("Getting image of camera :" + String(i));
    getCameraImage(i);
    //delay(5000);
    uploadCameraImage(i);
  }
  Serial.println(">>>>>End of main loop. Thread sleeps " + String(POOLING_INTERVAL) + " milleseconds...");
  setLedStates();
  for (int loop = 1000; loop < POOLING_INTERVAL; loop = loop + 1000) {
    delay(1000);
    setLedStates();
  }
}
