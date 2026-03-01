#include <WiFi.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WebSocketMCP.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include "DHT.h"

const char* ssid = "Duc";
const char* password = "123456@@@";

const char* mcpEndpoint =
"wss://api.xiaozhi.me/mcp/?token=eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9.eyJ1c2VySWQiOjU3NzA0MywiYWdlbnRJZCI6ODgwOTg4LCJlbmRwb2ludElkIjoiYWdlbnRfODgwOTg4IiwicHVycG9zZSI6Im1jcC1lbmRwb2ludCIsImlhdCI6MTc2OTU5Mzg0MiwiZXhwIjoxODAxMTUxNDQyfQ.CA8fkIbP6BIr6y2ekzVwWnSCurAe__371-12U0jCJA6ql6pXoh-a-xYI524KFBr9kKDP62o_5neP1YVzsfGXVA";

const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;

const char* topic_dht = "home/room1/dht";
const char* topic_light = "home/room1/light/state";
const char* topic_gas = "home/room1/gas/state";
const char* topic_mq135 = "home/room1/mq135";
const char* topic_pir = "home/room1/pir/state";
const char* topic_flame = "home/room1/flame/state";
const char* topic_seat = "domanhduc/room1/seat/status";  // Trang thai ghe tu Python (WORKING/COUNTDOWN/LEFT_SEAT)
const char* topic_light_control = "domanhduc/room1/light/control";  // Điều khiển đèn từ bên ngoài

#define DHTPIN 27
#define DHTTYPE DHT11
#define LIGHT_PIN 18         // Ngõ ra điều khiển relay đèn 220V
#define STATUS_LED_PIN 4     // LED hiển thị trạng thái đèn (LED mới ở D4)
#define MQ2_DO_PIN 23
#define MQ135_PIN 32
#define PIR_PIN 34
#define BUZZER_PIN 13
#define LED_PIR_PIN 2
#define FLAME_DO_PIN 26

DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 20, 4);

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
WebSocketMCP mcpClient;

// Cấu hình mức kích relay (đa số module relay cho ESP32 là ACTIVE LOW)
const bool RELAY_ON_LEVEL = LOW;
const bool RELAY_OFF_LEVEL = HIGH;

bool lightState = false;
bool gasAlert = false;
bool lastGasPublished = false;
bool flameDetected = false;
bool lastFlameState = false;
int lastMotion = LOW;
unsigned long lastMotionDetected = 0;
unsigned long pirAlertStartTime = 0;
const unsigned long PIR_ALERT_DURATION = 1500;

// Trang thai ghe tu Python (WORKING / COUNTDOWN / LEFT_SEAT)
String seatStatus = "UNKNOWN";
unsigned long lastSeatMsgTime = 0;

unsigned long lastPublish = 0;
unsigned long lastDHTRead = 0;
const unsigned long DHT_INTERVAL = 2000;

unsigned long lastMQ2Read = 0;
const unsigned long MQ2_INTERVAL = 500;

int mq135Value = 0;
unsigned long lastMQ135Read = 0;
const unsigned long MQ135_INTERVAL = 1000;
int lastMQ135Sent = -1;
unsigned long lastMQ135SentTime = 0;
const unsigned long MQ135_SEND_INTERVAL = 5000;  // Gửi tối đa mỗi 5 giây
const int MQ135_CHANGE_THRESHOLD = 30;  // Chỉ gửi khi thay đổi > 30

unsigned long lastLCD = 0;
const unsigned long LCD_INTERVAL = 1000;
float lastT = 0, lastH = 0;

unsigned long lastMqttReconnect = 0;
const unsigned long MQTT_RECONNECT_INTERVAL = 5000;

void setLight(bool on) {
  digitalWrite(LIGHT_PIN, on ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
  lightState = on;
  Serial.println(on ? ">>> LIGHT D18 ON (RELAY)" : ">>> LIGHT D18 OFF (RELAY)");
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (int i = 0; i < length; i++) message += (char)payload[i];
  Serial.print("[MQTT] ");
  Serial.print(topic);
  Serial.print(" => ");
  Serial.println(message);

  // Xu ly trang thai ghe gui tu Python
  if (String(topic) == topic_seat) {
    message.trim();
    // Xử lý COUNTDOWN có thể có format "COUNTDOWN:xx"
    if (message.startsWith("COUNTDOWN")) {
      seatStatus = "COUNTDOWN";
    } else {
      seatStatus = message;  // WORKING / LEFT_SEAT / UNKNOWN
    }
    lastSeatMsgTime = millis();
  }
  // Xu ly dieu khien den tu ben ngoai (dashboard, app, ...)
  else if (String(topic) == topic_light_control) {
    message.trim();
    message.toLowerCase();
    if (message == "on") {
      setLight(true);
    } else if (message == "off") {
      setLight(false);
    }
  }
}

void updateSeatLed(unsigned long now) {
  static unsigned long lastBlink = 0;
  static bool blinkState = false;

  // Nếu quá 10s không nhận dữ liệu từ Python -> tắt D4 để tránh sai
  if (now - lastSeatMsgTime > 10000) {
    digitalWrite(STATUS_LED_PIN, LOW);
    return;
  }

  if (seatStatus == "WORKING") {
    digitalWrite(STATUS_LED_PIN, HIGH);
  }
  else if (seatStatus == "COUNTDOWN") {
    // nháy chậm (1 giây đổi 1 lần)
    if (now - lastBlink > 1000) {
      lastBlink = now;
      blinkState = !blinkState;
      digitalWrite(STATUS_LED_PIN, blinkState ? HIGH : LOW);
    }
  }
  else if (seatStatus == "LEFT_SEAT") {
    // nháy nhanh cảnh báo
    if (now - lastBlink > 200) {
      lastBlink = now;
      blinkState = !blinkState;
      digitalWrite(STATUS_LED_PIN, blinkState ? HIGH : LOW);
    }
  }
  else {
    // trạng thái lạ -> tắt
    digitalWrite(STATUS_LED_PIN, LOW);
  }
}

String pad20(String s) {
  if (s.length() > 20) return s.substring(0, 20);
  while (s.length() < 20) s += " ";
  return s;
}

String seatShort(String st) {
  st.trim();
  st.toUpperCase();

  if (st.startsWith("COUNTDOWN")) return "COUNT";
  if (st == "WORKING") return "WORK ";
  if (st == "LEFT_SEAT") return "LEFT!";
  if (st == "UNKNOWN") return "UNKN ";
  return "---- ";
}

void updateLCD20x4(int motion) {
  // ====== Dòng 0: DHT ======
  float t = isnan(lastT) ? 0 : lastT;
  float h = isnan(lastH) ? 0 : lastH;

  String l0 = "T:" + String(t, 1) + "C H:" + String(h, 1) + "%";
  l0 = pad20(l0);

  // ====== Dòng 1: MQ2 + Flame ======
  String mq2Str = gasAlert ? "ALRT" : "SAFE";
  String flameStr = flameDetected ? "FIRE" : "OK  ";
  String l1 = "MQ2:" + mq2Str + " FL:" + flameStr;
  l1 = pad20(l1);

  // ====== Dòng 2: PIR + MQ135 ======
  String pirStr = (motion == HIGH) ? "YES" : "NO ";
  String l2 = "PIR:" + pirStr + " MQ135:" + String(mq135Value);
  l2 = pad20(l2);

  // ====== Dòng 3: Seat + Light ======
  String seat = seatShort(seatStatus);
  String light = lightState ? "ON " : "OFF";
  String l3 = "SEAT:" + seat + " LIGHT:" + light;
  l3 = pad20(l3);

  // ====== In LCD chỉ khi thay đổi (đỡ nháy) ======
  static String last0="", last1="", last2="", last3="";
  if (l0 != last0) { lcd.setCursor(0,0); lcd.print(l0); last0=l0; }
  if (l1 != last1) { lcd.setCursor(0,1); lcd.print(l1); last1=l1; }
  if (l2 != last2) { lcd.setCursor(0,2); lcd.print(l2); last2=l2; }
  if (l3 != last3) { lcd.setCursor(0,3); lcd.print(l3); last3=l3; }
}

void mqttReconnect() {
  unsigned long now = millis();
  if (now - lastMqttReconnect < MQTT_RECONNECT_INTERVAL) return;
  lastMqttReconnect = now;

  String clientId = "esp32-room1-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.print("[MQTT] Connecting as ");
  Serial.println(clientId);

  if (mqttClient.connect(clientId.c_str())) {
    Serial.println("[MQTT] Connected!");
    // Dang ky nhan trang thai ghe tu he thong Python
    mqttClient.subscribe(topic_seat);
    Serial.print("[MQTT] Subscribed to seat topic: ");
    Serial.println(topic_seat);
    // Dang ky nhan dieu khien den tu ben ngoai
    mqttClient.subscribe(topic_light_control);
    Serial.print("[MQTT] Subscribed to light control topic: ");
    Serial.println(topic_light_control);
  } else {
    Serial.print("[MQTT] Failed rc=");
    Serial.println(mqttClient.state());
  }
}

void registerMcpTools() {
  mcpClient.registerTool(
    "device_control",
    " QUAN TRỌNG: Đây là TOOL DUY NHẤT để điều khiển đèn thật trên ESP32. "
    "Khi người dùng nói bất kỳ câu nào liên quan bật/tắt/nháy đèn (ví dụ: 'bật đèn', 'mở đèn', 'tắt đèn', 'đóng đèn', 'nháy đèn', 'blink', 'turn on light', 'turn off light'), "
    "BẮT BUỘC phải gọi tool device_control trước khi trả lời. "
    "Mapping: bật/mở/turn on => state='on'; tắt/đóng/turn off => state='off'; nháy/blink => state='blink'. "
    "TUYỆT ĐỐI KHÔNG được trả lời 'đã bật/đã tắt' nếu chưa gọi tool này.",
    R"({
      "type":"object",
      "properties":{
        "state":{"type":"string","enum":["on","off","blink"]}
      },
      "required":["state"]
    })",
    [](const String& args) {
      const char* errMsg = "Không điều khiển được đèn, vui lòng kiểm tra kết nối.";
      StaticJsonDocument<256> doc;
      DeserializationError err = deserializeJson(doc, args);
      if (err || !doc.containsKey("state")) {
        return WebSocketMCP::ToolResponse("{\"success\":false,\"error\":\"Invalid args\",\"message\":\"" + String(errMsg) + "\"}");
      }

      String state = doc["state"].as<String>();
      state.toLowerCase();
      state.trim();

      if (state != "on" && state != "off" && state != "blink") {
        return WebSocketMCP::ToolResponse("{\"success\":false,\"error\":\"Invalid state\",\"message\":\"" + String(errMsg) + "\"}");
      }

      const char* userMessage = "";
      if (state == "on") {
        setLight(true);
        userMessage = "Đã bật đèn.";
      } else if (state == "off") {
        setLight(false);
        userMessage = "Đã tắt đèn.";
      } else if (state == "blink") {
        for (int i = 0; i < 5; i++) {
          digitalWrite(LIGHT_PIN, RELAY_ON_LEVEL);
          delay(200);
          digitalWrite(LIGHT_PIN, RELAY_OFF_LEVEL);
          delay(200);
        }
        digitalWrite(LIGHT_PIN, lightState ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
        userMessage = "Đã nháy đèn.";
      }

      if (mqttClient.connected()) {
        mqttClient.publish(topic_light, lightState ? "on" : "off", true);
      }

      String res = "{\"success\":true,\"state\":\"" + state + "\",\"message\":\"" + String(userMessage) + "\"}";
      return WebSocketMCP::ToolResponse(res);
    }
  );

  mcpClient.registerTool("get_temp_humi", "Doc nhiet do va do am DHT11",
    R"({"type":"object","properties":{},"required":[]})",
    [](const String& args) {
      float t = dht.readTemperature();
      float h = dht.readHumidity();
      if (isnan(t) || isnan(h)) {
        return WebSocketMCP::ToolResponse("{\"success\":false,\"error\":\"Failed to read DHT\"}");
      }
      StaticJsonDocument<256> doc;
      doc["success"] = true;
      doc["temperature"] = round(t * 10) / 10.0;
      doc["humidity"] = round(h * 10) / 10.0;
      String response;
      serializeJson(doc, response);
      return WebSocketMCP::ToolResponse(response);
    }
  );

  mcpClient.registerTool("get_gas", "Doc MQ2 gas state",
    R"({"type":"object","properties":{},"required":[]})",
    [](const String& args) {
      StaticJsonDocument<256> doc;
      doc["success"] = true;
      doc["gas"] = gasAlert ? "ALERT" : "SAFE";
      doc["pin"] = MQ2_DO_PIN;
      String response;
      serializeJson(doc, response);
      return WebSocketMCP::ToolResponse(response);
    }
  );

  mcpClient.registerTool("get_mq135", "Doc gia tri MQ135 (ADC raw 0-4095)",
    R"({"type":"object","properties":{},"required":[]})",
    [](const String& args) {
      StaticJsonDocument<256> doc;
      doc["success"] = true;
      doc["value"] = mq135Value;
      doc["pin"] = MQ135_PIN;
      String response;
      serializeJson(doc, response);
      return WebSocketMCP::ToolResponse(response);
    }
  );

  mcpClient.registerTool("get_pir", "Doc trang thai cam bien chuyen dong PIR",
    R"({"type":"object","properties":{},"required":[]})",
    [](const String& args) {
      int motion = digitalRead(PIR_PIN);
      StaticJsonDocument<512> doc;
      doc["success"] = true;
      doc["motion"] = (motion == HIGH) ? "MOTION" : "NO_MOTION";
      doc["pin"] = PIR_PIN;
      if (lastMotionDetected > 0) {
        unsigned long timeSinceMotion = millis() - lastMotionDetected;
        doc["lastMotionDetected"] = true;
        doc["secondsAgo"] = timeSinceMotion / 1000;
        doc["message"] = "Vua co nguoi chuyen dong qua cach day " + String(timeSinceMotion / 1000) + " giay";
      } else {
        doc["lastMotionDetected"] = false;
        doc["message"] = "Chua co chuyen dong nao duoc phat hien";
      }
      doc["ledOn"] = (pirAlertStartTime > 0);
      doc["buzzerOn"] = (pirAlertStartTime > 0);
      String response;
      serializeJson(doc, response);
      return WebSocketMCP::ToolResponse(response);
    }
  );

  mcpClient.registerTool("get_flame", "Doc trang thai cam bien lua (flame sensor DO)",
    R"({"type":"object","properties":{},"required":[]})",
    [](const String& args) {
      int doVal = digitalRead(FLAME_DO_PIN);
      bool flame = (doVal == LOW);
      StaticJsonDocument<256> doc;
      doc["success"] = true;
      doc["flame"] = flame ? "FLAME" : "NO_FLAME";
      doc["pin"] = FLAME_DO_PIN;
      String response;
      serializeJson(doc, response);
      return WebSocketMCP::ToolResponse(response);
    }
  );
}

void onConnectionStatus(bool connected) {
  if (connected) {
    Serial.println("[MCP] Connected");
    registerMcpTools();
  } else {
    Serial.println("[MCP] Disconnected");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(LIGHT_PIN, OUTPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);
  setLight(false);
  // Dam bao LED D4 tat ban dau
  digitalWrite(STATUS_LED_PIN, LOW);

  pinMode(MQ2_DO_PIN, INPUT);
  // MQ135_PIN là analog ADC, không cần pinMode
  pinMode(PIR_PIN, INPUT);
  pinMode(FLAME_DO_PIN, INPUT);
  ledcAttach(BUZZER_PIN, 2000, 8);
  pinMode(LED_PIR_PIN, OUTPUT);
  digitalWrite(LED_PIR_PIN, LOW);

  analogReadResolution(12);
  analogSetPinAttenuation(MQ135_PIN, ADC_11db);

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.print("Booting...");

  dht.begin();

  WiFi.begin(ssid, password);
  Serial.println("[WiFi] Connecting...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("[WiFi] Connected!");
  Serial.print("[WiFi] IP: ");
  Serial.println(WiFi.localIP());

  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);
  mcpClient.begin(mcpEndpoint, onConnectionStatus);

  lcd.clear();
  lcd.print("Ready");
  
  // Set lastSeatMsgTime để tránh D4 tắt ngay khi boot
  lastSeatMsgTime = millis();
}

void loop() {
  mcpClient.loop();
  if (!mqttClient.connected()) mqttReconnect();
  mqttClient.loop();

  unsigned long now = millis();

  if (now - lastMQ2Read >= MQ2_INTERVAL) {
    lastMQ2Read = now;
    int mq2 = digitalRead(MQ2_DO_PIN);
    gasAlert = (mq2 == LOW);
    if (mqttClient.connected() && gasAlert != lastGasPublished) {
      lastGasPublished = gasAlert;
      mqttClient.publish(topic_gas, gasAlert ? "ALERT" : "SAFE", true);
    }
  }

  if (now - lastMQ135Read >= MQ135_INTERVAL) {
    lastMQ135Read = now;
    mq135Value = analogRead(MQ135_PIN);
    // Chỉ publish khi thay đổi đáng kể hoặc đã quá 5 giây
    if (mqttClient.connected()) {
      bool shouldSend = false;
      if (lastMQ135Sent < 0) {
        // Lần đầu tiên luôn gửi
        shouldSend = true;
      } else {
        // Thay đổi > 30 đơn vị (dùng cách tính toán thủ công thay vì abs())
        int diff = mq135Value - lastMQ135Sent;
        if (diff < 0) diff = -diff;
        if (diff > MQ135_CHANGE_THRESHOLD) {
          shouldSend = true;
        }
      }
      if (!shouldSend && now - lastMQ135SentTime >= MQ135_SEND_INTERVAL) {
        // Đã quá 5 giây kể từ lần gửi cuối
        shouldSend = true;
      }
      
      if (shouldSend) {
        String mq135Str = String(mq135Value);
        mqttClient.publish(topic_mq135, mq135Str.c_str(), false);  // Không dùng retain để tránh spam
        lastMQ135Sent = mq135Value;
        lastMQ135SentTime = now;
      }
    }
  }

  // Debounce flame sensor đúng cách
  static bool lastFlameRaw = false;
  static unsigned long lastFlameChange = 0;
  bool flameRaw = (digitalRead(FLAME_DO_PIN) == LOW);
  
  if (flameRaw != lastFlameRaw) {
    lastFlameChange = now;
    lastFlameRaw = flameRaw;
  }
  
  if (now - lastFlameChange > 300) {
    flameDetected = flameRaw;
  }
  if (flameDetected != lastFlameState) {
    lastFlameState = flameDetected;
    if (flameDetected) {
      Serial.println("[FLAME] PHAT HIEN LUA !!!");
      ledcWrite(BUZZER_PIN, 128);
      if (mqttClient.connected()) {
        mqttClient.publish(topic_flame, "FLAME", true);
      }
    } else {
      Serial.println("[FLAME] An toan");
      if (pirAlertStartTime == 0) ledcWrite(BUZZER_PIN, 0);
      if (mqttClient.connected()) {
        mqttClient.publish(topic_flame, "NO_FLAME", true);
      }
    }
  }

  int motion = digitalRead(PIR_PIN);

  if (pirAlertStartTime > 0 && (now - pirAlertStartTime >= PIR_ALERT_DURATION)) {
    digitalWrite(LED_PIR_PIN, LOW);
    if (!flameDetected) ledcWrite(BUZZER_PIN, 0);
    pirAlertStartTime = 0;
  }

  if (motion != lastMotion) {
    lastMotion = motion;
    if (motion == HIGH) {
      Serial.println("[PIR] Co chuyen dong! Bat LED va buzzer trong 1.5s");
      lastMotionDetected = now;
      digitalWrite(LED_PIR_PIN, HIGH);
      if (!flameDetected) ledcWrite(BUZZER_PIN, 128);
      pirAlertStartTime = now;
      if (mqttClient.connected()) {
        mqttClient.publish(topic_pir, "MOTION", true);
      }
    } else {
      if (mqttClient.connected()) {
        mqttClient.publish(topic_pir, "NO_MOTION", true);
      }
    }
  }

  if (now - lastDHTRead >= DHT_INTERVAL) {
    lastDHTRead = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      lastT = t;
      lastH = h;
    }
    if (now - lastPublish > 5000 && !isnan(t) && !isnan(h)) {
      lastPublish = now;
      StaticJsonDocument<128> doc;
      doc["temperature"] = t;
      doc["humidity"] = h;
      char buf[128];
      serializeJson(doc, buf);
      mqttClient.publish(topic_dht, buf, true);
    }
  }

  if (now - lastLCD >= LCD_INTERVAL) {
    lastLCD = now;
    updateLCD20x4(motion);
  }

  // ===== LED D4 hiển thị trạng thái ghế =====
  updateSeatLed(now);

  delay(1);
}
