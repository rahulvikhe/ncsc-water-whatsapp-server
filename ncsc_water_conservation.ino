/*
==============================================================
 NCSC WATER CONSERVATION PROJECT
 "Differential Flow and Soil Moisture Correlation
  for Smart Water Conservation"

 ESP32 PROGRAM (SIMPLIFIED VERSION)

 HARDWARE
 -------------------------------------------------------------
 ESP32
 3 x YF-S401 flow sensors
 1 x soil-moisture sensor (in the main field)
 2 x 16x2 I2C LCDs
 NEO-6M GPS
 Relay module
 12V DC pump
 Wi-Fi

 PHYSICAL PIPELINE
 -------------------------------------------------------------
 Tank -> Pump -> FS1 -> Container 1 (demo leak section)
      -> FS2 -> Container 2 = MAIN FIELD (Soil Sensor 1)
      -> FS3 -> Outlet Bucket -> Return to Tank

 LEAK LOGIC
 -------------------------------------------------------------
 FS1 - FS2 = loss in first section
 FS2 - FS3 = loss in main-field section
 FS1 - FS3 = total system loss

 Soil moisture = supporting evidence (main field only)
 GPS           = location association
 Wi-Fi         = Internet communication
 Server        = WhatsApp communication
 Relay         = automatic pump protection

 IMPORTANT
 -------------------------------------------------------------
 1. Calibrate all three YF-S401 sensors experimentally.
 2. Do NOT connect a 5V sensor signal directly to an ESP32
    GPIO without level protection.
 3. Verify relay active HIGH/LOW for your actual relay.
 4. Check GPIO assignments against your exact ESP32 board.
==============================================================
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <TinyGPSPlus.h>

// ============================================================
// 1. USER CONFIGURATION
// ============================================================

const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

const char* API_URL         = "https://YOUR-SERVER-DOMAIN/api/leak-alert";
const char* API_COMMAND_URL = "https://YOUR-SERVER-DOMAIN/api/command";
const char* API_STARTED_URL = "https://YOUR-SERVER-DOMAIN/api/pump-started";
const char* API_TOKEN       = "YOUR_PRIVATE_ESP32_TOKEN";

// How often the ESP32 asks the server "is there a WhatsApp command?"
const unsigned long COMMAND_POLL_MS = 5000;

// ============================================================
// 2. GPIO CONFIGURATION
// ============================================================

// Flow sensors (input-only pins are fine)
const uint8_t FLOW1_PIN = 34;
const uint8_t FLOW2_PIN = 35;
const uint8_t FLOW3_PIN = 32;

// Soil sensor (ADC1 pin, works with Wi-Fi on)
const uint8_t SOIL1_PIN = 33;

// Relay
const uint8_t RELAY_PIN = 4;
const bool RELAY_ACTIVE_LOW = true;   // change if relay works opposite

// Manual pump start button: one side to GPIO 27, other side to GND
// (uses the ESP32 internal pull-up, no external resistor needed)
const uint8_t START_BUTTON_PIN = 27;

// GPS
const uint8_t GPS_RX_PIN = 16;
const uint8_t GPS_TX_PIN = 17;

// I2C
const uint8_t I2C_SDA = 21;
const uint8_t I2C_SCL = 22;

// ============================================================
// 3. LCD
// ============================================================

LiquidCrystal_I2C LCD1(0x27, 16, 2);
LiquidCrystal_I2C LCD2(0x26, 16, 2);

// ============================================================
// 4. GPS
// ============================================================

TinyGPSPlus gps;
HardwareSerial GPS_Serial(2);

// ============================================================
// 5. YF-S401 CALIBRATION
// ============================================================
// pulses_per_litre = measured_pulses / measured_litres
// Replace with YOUR experimental values.

float PULSES_PER_LITRE_1 = 5880.0;
float PULSES_PER_LITRE_2 = 5880.0;
float PULSES_PER_LITRE_3 = 5880.0;

// ============================================================
// 6. LIMITS
// ============================================================

const float MIN_VALID_FLOW = 0.30;
const float MAX_VALID_FLOW = 6.00;

// Minimum flow difference considered a loss (tune by experiment)
const float FLOW_LOSS_THRESHOLD = 0.50;

// Water loss (litres) that triggers pump shutdown
const float AUTO_SHUTDOWN_LOSS_LITRES = 2.00;

// Consecutive abnormal measurements needed
const uint8_t REQUIRED_CONFIRMATIONS = 5;

// ============================================================
// 7. SOIL CALIBRATION
// ============================================================
// dry soil -> record raw value, wet soil -> record raw value

const int SOIL_DRY_VALUE = 4095;
const int SOIL_WET_VALUE = 1200;

// Rise above baseline (in %) counted as supporting evidence
const float SOIL_RISE_THRESHOLD = 8.0;

const uint8_t SOIL_BASELINE_SAMPLES = 10;

// ============================================================
// 8. TIMING
// ============================================================

const unsigned long SENSOR_INTERVAL_MS = 1000;
const unsigned long LCD_INTERVAL_MS    = 2000;
const unsigned long WIFI_RETRY_MS      = 10000;
const unsigned long ALERT_COOLDOWN_MS  = 60000;

// ============================================================
// 9. FLOW VARIABLES
// ============================================================

volatile uint32_t pulseCount1 = 0;
volatile uint32_t pulseCount2 = 0;
volatile uint32_t pulseCount3 = 0;

float flow1 = 0.0, flow2 = 0.0, flow3 = 0.0;
float filteredFlow1 = 0.0, filteredFlow2 = 0.0, filteredFlow3 = 0.0;

// ============================================================
// 10. WATER VOLUME
// ============================================================

float totalInletLitres  = 0.0;
float totalMiddleLitres = 0.0;
float totalOutletLitres = 0.0;

float totalSystemLoss  = 0.0;
float section1LossRate = 0.0;
float section2LossRate = 0.0;

// ============================================================
// 11. SOIL VARIABLES (single sensor)
// ============================================================

int   soilRaw1      = 0;
float soilPercent1  = 0.0;
float soilBaseline1 = 0.0;

// ============================================================
// 12. GPS VARIABLES
// ============================================================

double latitude  = 0.0;
double longitude = 0.0;
bool   gpsFix    = false;

// ============================================================
// 13. SYSTEM STATE
// ============================================================

enum SystemStatus
{
  SYSTEM_STARTING,
  SYSTEM_NORMAL,
  PROBABLE_LEAK_SECTION_1,
  PROBABLE_LEAK_MAIN_FIELD,
  PUMP_SHUTDOWN
};

SystemStatus systemStatus = SYSTEM_STARTING;

uint8_t section1Confirmations = 0;
uint8_t section2Confirmations = 0;

bool pumpStopped = false;
bool alertSentForCurrentEvent = false;

bool alertPending = false;                       // shutdown alert waiting to be sent
SystemStatus pendingFaultSection = SYSTEM_NORMAL;

bool startNoticePending = false;                 // "pump started" notice waiting to be sent
const char* startSource = "BUTTON";              // BUTTON / SERIAL / WHATSAPP

unsigned long lastCommandPoll = 0;

unsigned long lastAlertTime   = 0;
unsigned long lastSensorTime  = 0;
unsigned long lastLCDTime     = 0;
unsigned long lastWiFiAttempt = 0;

// ============================================================
// 14. INTERRUPTS
// ============================================================

void IRAM_ATTR flow1ISR() { pulseCount1++; }
void IRAM_ATTR flow2ISR() { pulseCount2++; }
void IRAM_ATTR flow3ISR() { pulseCount3++; }

// ============================================================
// 15. SETUP
// ============================================================

void setup()
{
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("======================================");
  Serial.println("NCSC SMART WATER CONSERVATION");
  Serial.println("ESP32 SYSTEM STARTING");
  Serial.println("======================================");

  // Flow sensors
  pinMode(FLOW1_PIN, INPUT);
  pinMode(FLOW2_PIN, INPUT);
  pinMode(FLOW3_PIN, INPUT);

  attachInterrupt(digitalPinToInterrupt(FLOW1_PIN), flow1ISR, RISING);
  attachInterrupt(digitalPinToInterrupt(FLOW2_PIN), flow2ISR, RISING);
  attachInterrupt(digitalPinToInterrupt(FLOW3_PIN), flow3ISR, RISING);

  // Relay
  pinMode(RELAY_PIN, OUTPUT);
  pumpOn();

  // Manual start button
  pinMode(START_BUTTON_PIN, INPUT_PULLUP);

  // Soil
  pinMode(SOIL1_PIN, INPUT);

  // LCD
  Wire.begin(I2C_SDA, I2C_SCL);

  LCD1.init();
  LCD1.backlight();
  LCD2.init();
  LCD2.backlight();

  LCD1.clear();
  LCD2.clear();

  LCD1.setCursor(0, 0);
  LCD1.print("NCSC WATER");
  LCD1.setCursor(0, 1);
  LCD1.print("SMART SYSTEM");

  LCD2.setCursor(0, 0);
  LCD2.print("INITIALISING");
  LCD2.setCursor(0, 1);
  LCD2.print("PLEASE WAIT");

  // GPS
  GPS_Serial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  // Wi-Fi
  connectWiFi();

  // Baseline
  delay(2000);
  establishBaselines();

  systemStatus = SYSTEM_NORMAL;

  LCD1.clear();
  LCD2.clear();

  Serial.println("System initialisation complete.");
}

// ============================================================
// 16. MAIN LOOP
// ============================================================

void loop()
{
  readGPS();
  maintainWiFi();
  handleManualStart();

  // Shutdown alert (retries until the server accepts it)
  if (alertPending)
  {
    sendWhatsAppAlert(pendingFaultSection);
  }

  // "Pump started" confirmation
  if (startNoticePending)
  {
    sendPumpStartedNotice();
  }

  // Check for a START command sent from WhatsApp
  if (millis() - lastCommandPoll >= COMMAND_POLL_MS)
  {
    lastCommandPoll = millis();
    pollServerCommand();
  }

  if (millis() - lastSensorTime >= SENSOR_INTERVAL_MS)
  {
    lastSensorTime = millis();

    updateFlowMeasurements();
    readSoilSensor();
    analyseLeakage();
    printSystemData();
  }

  if (millis() - lastLCDTime >= LCD_INTERVAL_MS)
  {
    lastLCDTime = millis();
    updateLCD();
  }
}

// ============================================================
// 17. WIFI
// ============================================================

void connectWiFi()
{
  Serial.print("Connecting to Wi-Fi");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint8_t attempts = 0;

  while (WiFi.status() != WL_CONNECTED && attempts < 30)
  {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("Wi-Fi connected.");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  }
  else
  {
    Serial.println("Wi-Fi connection failed.");
  }
}

void maintainWiFi()
{
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWiFiAttempt < WIFI_RETRY_MS) return;

  lastWiFiAttempt = millis();

  Serial.println("Reconnecting Wi-Fi...");

  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// ============================================================
// 18. FLOW MEASUREMENT
// ============================================================

void updateFlowMeasurements()
{
  static unsigned long previousTime = 0;

  unsigned long currentTime = millis();

  if (previousTime == 0)
  {
    previousTime = currentTime;
    return;
  }

  float elapsedSeconds = (currentTime - previousTime) / 1000.0;
  previousTime = currentTime;

  if (elapsedSeconds <= 0) return;

  uint32_t p1, p2, p3;

  noInterrupts();
  p1 = pulseCount1;
  p2 = pulseCount2;
  p3 = pulseCount3;
  pulseCount1 = 0;
  pulseCount2 = 0;
  pulseCount3 = 0;
  interrupts();

  // Pulse frequency (Hz)
  float frequency1 = p1 / elapsedSeconds;
  float frequency2 = p2 / elapsedSeconds;
  float frequency3 = p3 / elapsedSeconds;

  // Frequency -> L/min
  flow1 = (frequency1 * 60.0) / PULSES_PER_LITRE_1;
  flow2 = (frequency2 * 60.0) / PULSES_PER_LITRE_2;
  flow3 = (frequency3 * 60.0) / PULSES_PER_LITRE_3;

  // Reject impossible values
  flow1 = constrain(flow1, 0.0, MAX_VALID_FLOW);
  flow2 = constrain(flow2, 0.0, MAX_VALID_FLOW);
  flow3 = constrain(flow3, 0.0, MAX_VALID_FLOW);

  // Exponential filter
  const float ALPHA = 0.35;

  filteredFlow1 = ALPHA * flow1 + (1.0 - ALPHA) * filteredFlow1;
  filteredFlow2 = ALPHA * flow2 + (1.0 - ALPHA) * filteredFlow2;
  filteredFlow3 = ALPHA * flow3 + (1.0 - ALPHA) * filteredFlow3;

  // Volumes
  totalInletLitres  += filteredFlow1 * elapsedSeconds / 60.0;
  totalMiddleLitres += filteredFlow2 * elapsedSeconds / 60.0;
  totalOutletLitres += filteredFlow3 * elapsedSeconds / 60.0;

  // Section losses
  section1LossRate = filteredFlow1 - filteredFlow2;
  section2LossRate = filteredFlow2 - filteredFlow3;

  if (section1LossRate < 0) section1LossRate = 0;
  if (section2LossRate < 0) section2LossRate = 0;

  // Total loss
  totalSystemLoss = totalInletLitres - totalOutletLitres;

  if (totalSystemLoss < 0) totalSystemLoss = 0;
}

// ============================================================
// 19. SOIL MOISTURE (single sensor)
// ============================================================

float soilRawToPercentage(int rawValue)
{
  float percentage =
    100.0 * (SOIL_DRY_VALUE - rawValue) /
    (float)(SOIL_DRY_VALUE - SOIL_WET_VALUE);

  return constrain(percentage, 0.0, 100.0);
}

void readSoilSensor()
{
  soilRaw1     = analogRead(SOIL1_PIN);
  soilPercent1 = soilRawToPercentage(soilRaw1);
}

// ============================================================
// 20. BASELINE
// ============================================================

void establishBaselines()
{
  Serial.println();
  Serial.println("Establishing soil baseline...");

  float soilSum = 0;

  for (uint8_t i = 0; i < SOIL_BASELINE_SAMPLES; i++)
  {
    soilSum += soilRawToPercentage(analogRead(SOIL1_PIN));
    delay(100);
  }

  soilBaseline1 = soilSum / SOIL_BASELINE_SAMPLES;

  Serial.println("Baseline established.");
}

// ============================================================
// 21. LEAK ANALYSIS
// ============================================================

void analyseLeakage()
{
  // Do not diagnose when inlet flow is below the useful range
  if (filteredFlow1 < MIN_VALID_FLOW)
  {
    systemStatus = pumpStopped ? PUMP_SHUTDOWN : SYSTEM_NORMAL;

    section1Confirmations = 0;
    section2Confirmations = 0;

    return;
  }

  // Differential flow evidence
  bool section1FlowLoss = section1LossRate >= FLOW_LOSS_THRESHOLD;
  bool section2FlowLoss = section2LossRate >= FLOW_LOSS_THRESHOLD;

  // Soil supporting evidence (main field only)
  bool mainFieldSoilEvidence =
    soilPercent1 > soilBaseline1 + SOIL_RISE_THRESHOLD;

  // Section 1 (FS1 -> FS2): flow evidence only
  if (section1FlowLoss)
  {
    section1Confirmations++;
  }
  else if (section1Confirmations > 0)
  {
    section1Confirmations--;
  }

  // Section 2 / Main Field (FS2 -> FS3): flow + soil support
  if (section2FlowLoss)
  {
    if (mainFieldSoilEvidence)
      section2Confirmations += 2;
    else
      section2Confirmations++;
  }
  else if (section2Confirmations > 0)
  {
    section2Confirmations--;
  }

  // Limit counters
  if (section1Confirmations > REQUIRED_CONFIRMATIONS)
    section1Confirmations = REQUIRED_CONFIRMATIONS;

  if (section2Confirmations > REQUIRED_CONFIRMATIONS)
    section2Confirmations = REQUIRED_CONFIRMATIONS;

  // Determine location
  if (section1Confirmations >= REQUIRED_CONFIRMATIONS)
  {
    systemStatus = PROBABLE_LEAK_SECTION_1;
  }
  else if (section2Confirmations >= REQUIRED_CONFIRMATIONS)
  {
    systemStatus = PROBABLE_LEAK_MAIN_FIELD;
  }
  else
  {
    systemStatus = SYSTEM_NORMAL;
  }

  // Automatic pump protection
  if (
    systemStatus != SYSTEM_NORMAL &&
    totalSystemLoss >= AUTO_SHUTDOWN_LOSS_LITRES &&
    !pumpStopped
  )
  {
    // Remember which section was at fault before the status changes
    SystemStatus faultSection = systemStatus;

    stopPump();

    // Queue the alert: it is retried automatically until it is accepted
    alertPending = true;
    pendingFaultSection = faultSection;
  }
}

// ============================================================
// 22. PUMP CONTROL
// ============================================================

void pumpOn()
{
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
  pumpStopped = false;
}

// Restart the pump manually after a shutdown and clear the old leak event.
// Triggered by the push button (GPIO 27) or by typing 'S' in the Serial Monitor.
void manualStartPump()
{
  if (!pumpStopped) return;

  Serial.println();
  Serial.println(">>> MANUAL PUMP START <<<");

  // Clear old leak event so it does not instantly re-trigger
  totalInletLitres  = 0.0;
  totalMiddleLitres = 0.0;
  totalOutletLitres = 0.0;
  totalSystemLoss   = 0.0;

  section1Confirmations = 0;
  section2Confirmations = 0;

  alertSentForCurrentEvent = false;
  alertPending = false;

  systemStatus = SYSTEM_NORMAL;

  pumpOn();

  startNoticePending = true;
}

void handleManualStart()
{
  static bool lastButtonState = HIGH;
  static unsigned long lastChangeTime = 0;

  // Serial command: send 'S' to start the pump
  if (Serial.available())
  {
    char c = Serial.read();
    if (c == 'S' || c == 's') { startSource = "SERIAL"; manualStartPump(); }
  }

  // Button with 50 ms debounce (pressed = LOW)
  bool state = digitalRead(START_BUTTON_PIN);

  if (state != lastButtonState && millis() - lastChangeTime > 50)
  {
    lastChangeTime = millis();
    lastButtonState = state;

    if (state == LOW) { startSource = "BUTTON"; manualStartPump(); }
  }
}

void stopPump()
{
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? LOW : HIGH);

  pumpStopped = true;
  systemStatus = PUMP_SHUTDOWN;

  Serial.println();
  Serial.println("!!! AUTOMATIC PUMP SHUTDOWN !!!");
}

// ============================================================
// 23. GPS
// ============================================================

void readGPS()
{
  while (GPS_Serial.available())
  {
    gps.encode(GPS_Serial.read());
  }

  if (gps.location.isValid() && gps.location.age() < 10000)
  {
    latitude  = gps.location.lat();
    longitude = gps.location.lng();
    gpsFix    = true;
  }
  else
  {
    gpsFix = false;
  }
}

// ============================================================
// 24. WHATSAPP SERVER REQUEST
// ============================================================

void sendWhatsAppAlert(SystemStatus faultSection)
{
  if (alertSentForCurrentEvent) return;

  if (millis() - lastAlertTime < ALERT_COOLDOWN_MS && lastAlertTime != 0)
    return;

  // Wi-Fi down: stay pending and retry silently once it is back
  if (WiFi.status() != WL_CONNECTED) return;

  lastAlertTime = millis();

  // Leak section text
  String leakSection;

  if (faultSection == PROBABLE_LEAK_SECTION_1)
    leakSection = "Section 1: FS1 to FS2 / Container 1";
  else if (faultSection == PROBABLE_LEAK_MAIN_FIELD)
    leakSection = "Section 2: FS2 to FS3 / Main Field";
  else
    leakSection = "System";

  // JSON
  String json = "{";

  json += "\"project\":\"NCSC Smart Water Conservation\",";
  json += "\"status\":\"PROBABLE_LEAK\",";
  json += "\"section\":\"" + leakSection + "\",";
  json += "\"flow1\":" + String(filteredFlow1, 3) + ",";
  json += "\"flow2\":" + String(filteredFlow2, 3) + ",";
  json += "\"flow3\":" + String(filteredFlow3, 3) + ",";
  json += "\"section1LossRate\":" + String(section1LossRate, 3) + ",";
  json += "\"section2LossRate\":" + String(section2LossRate, 3) + ",";
  json += "\"waterLoss\":" + String(totalSystemLoss, 3) + ",";
  json += "\"soil1\":" + String(soilPercent1, 1) + ",";
  json += "\"latitude\":" + String(latitude, 6) + ",";
  json += "\"longitude\":" + String(longitude, 6) + ",";
  json += "\"gpsFix\":" + String(gpsFix ? "true" : "false") + ",";
  json += "\"pump\":\"STOPPED\"";

  json += "}";

  // HTTPS
  WiFiClientSecure client;

  // Prototype only: skips certificate verification.
  // For production, install and verify the server CA certificate.
  client.setInsecure();

  HTTPClient http;

  if (!http.begin(client, API_URL))
  {
    Serial.println("Could not connect to API.");
    return;
  }

  http.setTimeout(10000);

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + API_TOKEN);

  int responseCode = http.POST(json);

  Serial.print("API response: ");
  Serial.println(responseCode);
  Serial.println(http.getString());

  http.end();

  if (responseCode >= 200 && responseCode < 300)
  {
    alertSentForCurrentEvent = true;
    alertPending = false;
    Serial.println("WhatsApp alert request accepted.");
  }
}

// ============================================================
// 24b. WHATSAPP COMMANDS (via your server)
// ============================================================

// Asks the server whether you sent "START" on WhatsApp.
void pollServerCommand()
{
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();   // prototype only

  HTTPClient http;

  if (!http.begin(client, API_COMMAND_URL)) return;

  http.setTimeout(5000);
  http.addHeader("Authorization", String("Bearer ") + API_TOKEN);

  int code = http.GET();

  if (code == 200)
  {
    String body = http.getString();

    if (body.indexOf("START") >= 0)
    {
      Serial.println("START command received from WhatsApp.");

      if (pumpStopped)
      {
        startSource = "WHATSAPP";
        manualStartPump();
      }
      else
      {
        Serial.println("Pump already running - command ignored.");
      }
    }
  }

  http.end();
}

// Tells the server the pump was started so it can message you.
void sendPumpStartedNotice()
{
  static unsigned long lastTry = 0;

  if (WiFi.status() != WL_CONNECTED) return;
  if (lastTry != 0 && millis() - lastTry < 10000) return;
  lastTry = millis();

  WiFiClientSecure client;
  client.setInsecure();   // prototype only

  HTTPClient http;

  if (!http.begin(client, API_STARTED_URL)) return;

  http.setTimeout(10000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + API_TOKEN);

  String json = "{\"event\":\"PUMP_STARTED\",\"source\":\"";
  json += startSource;
  json += "\"}";

  int code = http.POST(json);

  http.end();

  if (code >= 200 && code < 300)
  {
    startNoticePending = false;
  }
}

// ============================================================
// 25. LCD
// ============================================================

void updateLCD()
{
  LCD1.clear();

  LCD1.setCursor(0, 0);
  LCD1.print("IN:");
  LCD1.print(filteredFlow1, 1);
  LCD1.print(" OUT:");
  LCD1.print(filteredFlow3, 1);

  LCD1.setCursor(0, 1);
  LCD1.print("LOSS:");
  LCD1.print(totalSystemLoss, 1);
  LCD1.print("L");

  LCD2.clear();

  LCD2.setCursor(0, 0);

  if (pumpStopped)
    LCD2.print("PUMP: STOPPED");
  else if (systemStatus == PROBABLE_LEAK_SECTION_1)
    LCD2.print("LEAK: SEC-1");
  else if (systemStatus == PROBABLE_LEAK_MAIN_FIELD)
    LCD2.print("LEAK: FIELD");
  else
    LCD2.print("SYSTEM NORMAL");

  LCD2.setCursor(0, 1);
  LCD2.print("SOIL:");
  LCD2.print(soilPercent1, 0);
  LCD2.print("%");
}

// ============================================================
// 26. SERIAL MONITOR
// ============================================================

void printSystemData()
{
  Serial.println();
  Serial.println("========== NCSC SYSTEM DATA ==========");

  Serial.print("FS1: ");
  Serial.print(filteredFlow1, 3);
  Serial.println(" L/min");

  Serial.print("FS2: ");
  Serial.print(filteredFlow2, 3);
  Serial.println(" L/min");

  Serial.print("FS3: ");
  Serial.print(filteredFlow3, 3);
  Serial.println(" L/min");

  Serial.print("Section 1 loss: ");
  Serial.print(section1LossRate, 3);
  Serial.println(" L/min");

  Serial.print("Section 2 loss: ");
  Serial.print(section2LossRate, 3);
  Serial.println(" L/min");

  Serial.print("Total water loss: ");
  Serial.print(totalSystemLoss, 3);
  Serial.println(" L");

  Serial.print("Soil: ");
  Serial.print(soilPercent1, 1);
  Serial.println("%");

  Serial.print("GPS: ");

  if (gpsFix)
  {
    Serial.print(latitude, 6);
    Serial.print(", ");
    Serial.println(longitude, 6);
  }
  else
  {
    Serial.println("NO FIX");
  }

  Serial.print("Wi-Fi: ");
  Serial.println(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");

  Serial.print("STATUS: ");

  switch (systemStatus)
  {
    case SYSTEM_NORMAL:
      Serial.println("NORMAL");
      break;
    case PROBABLE_LEAK_SECTION_1:
      Serial.println("PROBABLE LEAK - SECTION 1");
      break;
    case PROBABLE_LEAK_MAIN_FIELD:
      Serial.println("PROBABLE LEAK - MAIN FIELD");
      break;
    case PUMP_SHUTDOWN:
      Serial.println("PUMP AUTOMATICALLY STOPPED");
      break;
    default:
      Serial.println("STARTING");
  }

  Serial.println("======================================");
}
