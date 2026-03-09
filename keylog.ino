#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h> // Обов'язково версії 6.x
#include <LittleFS.h>
#include <time.h>

// --- АПАРАТНІ НАЛАШТУВАННЯ ESP32 ---
const int NUM_PORTS = 4;
// Найкращі безпечні піни для ESP32 30-pin: D25, D26, D27, D32
const int KEY_PINS[NUM_PORTS] = {25, 26, 27, 32}; 
// Кнопка BOOT на платі ESP32 (GPIO 0) для апаратного скидання
const int RESET_PIN = 0; 

// --- ГЛОБАЛЬНІ ЗМІННІ (Зберігаються в LittleFS) ---
String adminUser = "admin";
String adminPass = "admin";
String botToken = "";
String chatID = "";
String keyNames[NUM_PORTS] = {"Ключ 1", "Ключ 2", "Ключ 3", "Ключ 4"};

// --- СТАНИ ТА ІСТОРІЯ ---
int currentKeyState[NUM_PORTS];
int lastReading[NUM_PORTS];
unsigned long lastDebounceTime[NUM_PORTS];
const unsigned long debounceDelay = 50; // Антибрязкіт 50мс

const int HIST_SIZE = 15;
String history[HIST_SIZE];
int histHead = 0;
int histCount = 0;

unsigned long buttonPressTime = 0;
bool isResetting = false;

// Часовий пояс для України (з автоматичним переходом літо/зима)
const char* MYTZ = "EET-2EEST,M3.5.0/3,M10.5.0/4";

// --- ОБ'ЄКТИ ---
WebServer server(80);
WiFiClientSecure secured_client;
UniversalTelegramBot* bot = nullptr;

// --- ОТРИМАННЯ ФОРМАТОВАНОГО ЧАСУ ---
String getTimeString() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "[" + String(millis() / 1000) + "s uptime]";
  }
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%H:%M:%S %d.%m", &timeinfo);
  return "[" + String(buffer) + "]";
}

// --- ДОДАВАННЯ В ІСТОРІЮ ---
void addHistory(String event) {
  history[histHead] = getTimeString() + " " + event;
  histHead = (histHead + 1) % HIST_SIZE;
  if (histCount < HIST_SIZE) histCount++;
}

// --- РОБОТА З LittleFS ---
void saveConfig() {
  DynamicJsonDocument doc(1024);
  doc["au"] = adminUser; 
  doc["ap"] = adminPass;
  doc["bt"] = botToken; 
  doc["ci"] = chatID;
  JsonArray names = doc.createNestedArray("kn");
  for (int i = 0; i < NUM_PORTS; i++) names.add(keyNames[i]);
  
  File file = LittleFS.open("/cfg.json", "w");
  if (file) { 
    serializeJson(doc, file); 
    file.close(); 
    Serial.println("Налаштування збережено.");
  }
}

void loadConfig() {
  if (LittleFS.exists("/cfg.json")) {
    File file = LittleFS.open("/cfg.json", "r");
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, file);
    if (!error) {
      adminUser = doc["au"].as<String>(); 
      adminPass = doc["ap"].as<String>();
      botToken = doc["bt"].as<String>(); 
      chatID = doc["ci"].as<String>();
      for (int i = 0; i < NUM_PORTS; i++) {
        keyNames[i] = doc["kn"][i].as<String>();
      }
      Serial.println("Налаштування завантажено.");
    }
    file.close();
  }
}

// --- ВЕБ-ІНТЕРФЕЙС (MODERN UI) ---
void handleRoot() {
  if (!server.authenticate(adminUser.c_str(), adminPass.c_str())) {
    return server.requestAuthentication();
  }

  String s = R"rawliteral(
<!DOCTYPE html><html lang="uk"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>PortKey Dashboard</title>
<style>
:root{--bg:#121212;--card:#1e1e1e;--text:#e0e0e0;--primary:#00bcd4;--in:#4caf50;--out:#f44336;--border:rgba(255,255,255,0.05);}
body{font-family:'Segoe UI',Roboto,sans-serif;background:var(--bg);color:var(--text);margin:0;padding:10px;display:flex;justify-content:center;}
.co{max-width:500px;width:100%;}
h2{color:var(--primary);font-weight:300;border-bottom:1px solid var(--border);padding-bottom:10px;margin-top:30px;display:flex;align-items:center;justify-content:space-between;}
.refresh-info{font-size:12px;color:#666;}
.gr{display:grid;grid-template-columns:1fr 1fr;gap:15px;}
.ca{background:var(--card);padding:20px;border-radius:15px;border:1px solid var(--border);box-shadow:0 10px 20px rgba(0,0,0,0.2);transition:0.3s;}
.ki{text-align:center;position:relative;overflow:hidden;}
.ki::after{content:'';position:absolute;bottom:0;left:0;width:100%;height:4px;}
.s-in{color:var(--in);}.s-in::after{background:var(--in);}
.s-out{color:var(--out);}.s-out::after{background:var(--out);}
.kn{font-size:18px;font-weight:600;margin-bottom:5px;color:var(--text);}
.ks{font-size:12px;text-transform:uppercase;letter-spacing:1px;font-weight:bold;}
.hi{font-family:monospace;font-size:13px;color:#aaa;max-height:200px;overflow-y:auto;background:rgba(0,0,0,0.2);padding:10px;border-radius:10px;}
.line{border-bottom:1px solid rgba(255,255,255,0.03);padding:4px 0;}
.time{color:var(--primary);margin-right:5px;}
form{background:var(--card);padding:20px;border-radius:15px;border:1px solid var(--border);}
label{display:block;font-size:13px;color:#999;margin-bottom:5px;margin-top:15px;}
input{width:100%;padding:12px;background:rgba(255,255,255,0.03);border:1px solid var(--border);border-radius:8px;color:var(--text);box-sizing:border-box;font-size:15px;}
input:focus{outline:none;border-color:var(--primary);}
button{width:100%;padding:15px;margin-top:25px;border:none;border-radius:8px;font-size:16px;font-weight:bold;cursor:pointer;transition:0.2s;}
.b-p{background:var(--primary);color:#121212;}
.b-p:hover{background:#00acc1;}
.b-d{background:#2c2c2c;color:var(--out);margin-top:10px;font-size:14px;padding:10px;}
.b-d:hover{background:#3d3d3d;}
@media(max-width:400px){.gr{grid-template-columns:1fr;}}
</style>
<script>setTimeout(()=>location.reload(),15000);</script>
</head><body><div class="co">
<h2>PortKey <span class="refresh-info">оновлення ~15с</span></h2>
<div class="gr">)rawliteral";

  for (int i = 0; i < NUM_PORTS; i++) {
    bool isOut = (currentKeyState[i] == HIGH);
    s += "<div class='ca ki " + String(isOut ? "s-out" : "s-in") + "'>";
    s += "<div class='kn'>" + keyNames[i] + "</div>";
    s += "<div class='ks'>" + String(isOut ? "🛑 Забрано" : "✅ На місці") + "</div>";
    s += "<div style='font-size:10px; color:#555; margin-top:5px;'>port D" + String(KEY_PINS[i]) + "</div></div>";
  }

  s += R"rawliteral(</div><h2>📜 Історія подій</h2><div class="ca hi">)rawliteral";

  for (int i = 0; i < histCount; i++) {
    int idx = (histHead - 1 - i + HIST_SIZE) % HIST_SIZE;
    String entry = history[idx];
    int bracketEnd = entry.indexOf(']');
    s += "<div class='line'><span class='time'>" + entry.substring(0, bracketEnd + 1) + "</span>";
    s += entry.substring(bracketEnd + 1) + "</div>";
  }

  s += R"rawliteral(</div><h2>⚙️ Налаштування</h2><form action="/save" method="POST">)rawliteral";

  for (int i = 0; i < NUM_PORTS; i++) {
    s += "<label>Назва ключа " + String(i + 1) + "</label><input type='text' name='k" + String(i) + "' value='" + keyNames[i] + "' maxlength='20'>";
  }

  s += "<label>Логін адмінки</label><input type='text' name='au' value='" + adminUser + "'>";
  s += "<label>Пароль адмінки</label><input type='password' name='ap' value='" + adminPass + "'>";
  s += "<label>Telegram Token</label><input type='password' name='bt' value='" + botToken + "'>";
  s += "<label>Telegram Chat ID</label><input type='text' name='ci' value='" + chatID + "'>";
  s += "<button type='submit' class='b-p'>💾 Зберегти зміни</button></form>";
  s += "<form action='/reset' method='POST' onsubmit='return confirm(\"Повністю скинути пристрій? Усі налаштування будуть видалені.\")'><button type='submit' class='b-d'>⚠️ Скинути WiFi та налаштування</button></form>";
  s += "<div style='text-align:center;color:#444;font-size:10px;margin:20px 0;'>PortKey ESP32 Core | Time: "+getTimeString()+"</div></div></body></html>";

  server.send(200, "text/html", s);
}

void handleSave() {
  if (!server.authenticate(adminUser.c_str(), adminPass.c_str())) return server.requestAuthentication();
  
  for (int i = 0; i < NUM_PORTS; i++) { 
    if (server.hasArg("k" + String(i))) keyNames[i] = server.arg("k" + String(i)); 
  }
  if (server.hasArg("au") && server.arg("au") != "") adminUser = server.arg("au");
  if (server.hasArg("ap") && server.arg("ap") != "") adminPass = server.arg("ap");
  if (server.hasArg("bt")) botToken = server.arg("bt"); 
  if (server.hasArg("ci")) chatID = server.arg("ci");
  
  saveConfig();
  
  // Перезапуск об'єкта бота з новим токеном
  if (bot != nullptr) delete bot;
  if (botToken.length() > 5) bot = new UniversalTelegramBot(botToken, secured_client);
  
  server.sendHeader("Location", "/"); 
  server.send(303);
}

void handleReset() {
  if (!server.authenticate(adminUser.c_str(), adminPass.c_str())) return server.requestAuthentication();
  server.send(200, "text/plain", "Виконується скидання... Точка доступу PortKey_Setup з'явиться через декілька секунд.");
  delay(1000);
  LittleFS.format(); 
  WiFi.disconnect(true, true); 
  delay(500); 
  ESP.restart();
}

// --- SETUP ---
void setup() {
  Serial.begin(115200);
  Serial.println("\n--- Запуск PortKey ESP32 ---");
  
  pinMode(RESET_PIN, INPUT_PULLUP);
  
  // Ініціалізація LittleFS (true = відформатувати при помилці)
  if (!LittleFS.begin(true)) { 
    Serial.println("Помилка монтування LittleFS"); 
    return; 
  }
  loadConfig();
  
  // Налаштування пінів
  for (int i = 0; i < NUM_PORTS; i++) { 
    pinMode(KEY_PINS[i], INPUT_PULLUP); 
    currentKeyState[i] = digitalRead(KEY_PINS[i]); 
    lastReading[i] = currentKeyState[i]; 
  }

  // Запуск WiFiManager
  WiFiManager wifiManager;
  wifiManager.setAPCallback([](WiFiManager* myWiFiManager) { 
    Serial.println("Увімкнено режим точки доступу: PortKey_Setup"); 
  });
  
  // Якщо не вдалося підключитися, створюємо AP "PortKey_Setup"
  if (!wifiManager.autoConnect("PortKey_Setup")) { 
    Serial.println("Не вдалося підключитися. Перезавантаження...");
    delay(3000);
    ESP.restart(); 
  }

  Serial.println("WiFi підключено. IP: " + WiFi.localIP().toString());

  // Налаштування часу для України
  configTzTime(MYTZ, "pool.ntp.org", "time.nist.gov");
  
  // Дозвіл на підключення без перевірки сертифікату сервера
  secured_client.setInsecure();
  
  if (botToken.length() > 5) {
    bot = new UniversalTelegramBot(botToken, secured_client);
    if (chatID.length() > 3) {
      bot->sendMessage(chatID, "✅ PortKey підключено до мережі і готово до роботи!", "");
    }
  }
  
  // Налаштування веб-сервера
  server.on("/", handleRoot); 
  server.on("/save", HTTP_POST, handleSave); 
  server.on("/reset", HTTP_POST, handleReset);
  server.begin();
  
  addHistory("Система запущена");
}

// --- LOOP ---
void loop() {
  server.handleClient();
  
  // Обробка апаратної кнопки скидання (кнопка BOOT на ESP32)
  if (digitalRead(RESET_PIN) == LOW) {
    if (buttonPressTime == 0) buttonPressTime = millis();
    // Якщо затиснуто понад 3 секунди
    if (millis() - buttonPressTime > 3000 && !isResetting) { 
      isResetting = true; 
      Serial.println("Апаратне скидання активовано!");
      LittleFS.format(); 
      WiFi.disconnect(true, true); 
      delay(500); 
      ESP.restart();
    }
  } else { 
    buttonPressTime = 0; 
  }

  // Опитування пінів ключів
  for (int i = 0; i < NUM_PORTS; i++) {
    int reading = digitalRead(KEY_PINS[i]);
    
    if (reading != lastReading[i]) {
      lastDebounceTime[i] = millis();
    }
    
    if ((millis() - lastDebounceTime[i]) > debounceDelay) {
      if (reading != currentKeyState[i]) {
        currentKeyState[i] = reading;
        String msg = "";
        
        if (currentKeyState[i] == HIGH) {
          msg = "🔴 eth" + String(i + 1) + " DOWN\nКлюч '" + keyNames[i] + "' забрали";
          addHistory("Видано: " + keyNames[i]);
        } else {
          msg = "🟢 eth" + String(i + 1) + " UP\nКлюч '" + keyNames[i] + "' повернуто";
          addHistory("Повернуто: " + keyNames[i]);
        }
        
        Serial.println(msg);
        
        // Відправка в Telegram
        if (bot != nullptr && chatID.length() > 3) {
          bot->sendMessage(chatID, msg, "");
        }
      }
    }
    lastReading[i] = reading;
  }
}