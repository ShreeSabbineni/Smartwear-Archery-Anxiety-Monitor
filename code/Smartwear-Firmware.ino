#include <WiFi.h>
#include <WebServer.h>

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "MAX30105.h"
#include "I2Cdev.h"
#include "MPU6050_6Axis_MotionApps612.h"

// ---------------- WIFI ----------------
const char* ssid = "Sensor";
const char* password = "123456";
WebServer server(80);

// ---------------- CONSTANTS ----------------
#define SAMPLE_INTERVAL_MS 10
#define FINGER_THRESHOLD 50000
#define AC_THRESHOLD 200
#define BUFFER_SIZE 25
#define RGB_BUILTIN 2
#define INTERRUPT_PIN 8

#define GSR_PIN 4   // ESP32 S3 analog pin

// ---------------- OLED ----------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define SCREEN_ADDRESS 0x3c
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ---------------- MAX30102 ----------------
MAX30105 particleSensor;
long irBuffer[BUFFER_SIZE];
int bufferIndex = 0;
long rollingAvg = 0;
bool lastBeatState = false;
unsigned long lastBeatTime = 0;
float bpm = 0;

// ---------------- MPU6050 ----------------
MPU6050 mpu;
volatile bool mpuInterrupt = false;
bool dmpReady = false;
uint16_t packetSize;
uint8_t fifoBuffer[64];
Quaternion q;
VectorFloat gravity;
float ypr[3];

void dmpDataReady() {
  mpuInterrupt = true;
}

// ---------------- GSR FILTER ----------------
#define GSR_BUFFER 20
int gsrBuffer[GSR_BUFFER];
int gsrIndex = 0;
long gsrSum = 0;
int filteredGSR = 0;

// ---------------- BASELINE ----------------
int gsrBaseline = 0;
bool baselineReady = false;
unsigned long baselineStart;
long baselineSum = 0;
int baselineCount = 0;

float anxietyPercent = 0;

// ---------------- HTML ----------------
String getHTML() {
  return R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
<style>
body { background:#111; color:white; text-align:center; font-family:Arial; }
.card { background:#222; padding:15px; margin:10px; border-radius:10px; }
</style>
</head>
<body>

<h2>ESP32 Biometric Dashboard</h2>

<div class="card">BPM: <span id="bpm">0</span></div>
<div class="card">GSR: <span id="gsr">0</span></div>
<div class="card">Anxiety: <span id="anx">0</span>%</div>
<div class="card" id="msg">--</div>

<canvas id="chart"></canvas>

<script>
let chart = new Chart(document.getElementById("chart"), {
 type:'line',
 data:{labels:[],datasets:[{label:'Anxiety',data:[]}]}
});

setInterval(()=>{
 fetch('/data').then(r=>r.json()).then(d=>{
  bpm.innerText=d.bpm;
  gsr.innerText=d.gsr;
  anx.innerText=d.anxiety;

  let msg="Calm";
  if(d.anxiety>60) msg="High Stress! Relax";
  else if(d.anxiety>30) msg="Mild Stress";
  document.getElementById("msg").innerText=msg;

  chart.data.labels.push('');
  chart.data.datasets[0].data.push(d.anxiety);
  if(chart.data.labels.length>20){
    chart.data.labels.shift();
    chart.data.datasets[0].data.shift();
  }
  chart.update();
 });
},1000);
</script>

</body>
</html>
)rawliteral";
}

// ---------------- ROUTES ----------------
void handleRoot() {
  server.send(200, "text/html", getHTML());
}

void handleData() {
  String json = "{";
  json += "\"bpm\":" + String(bpm,1) + ",";
  json += "\"gsr\":" + String(filteredGSR) + ",";
  json += "\"anxiety\":" + String(anxietyPercent,1);
  json += "}";
  server.send(200, "application/json", json);
}

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin(6, 7);
  Wire.setClock(400000);

  pinMode(GSR_PIN, INPUT);

  // OLED
  display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
  display.clearDisplay();

  // MAX30102
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30102 not found");
    while (1);
  }
  particleSensor.setup();

  // MPU6050
  mpu.initialize();
  pinMode(INTERRUPT_PIN, INPUT);
  mpu.dmpInitialize();
  mpu.setDMPEnabled(true);
  attachInterrupt(digitalPinToInterrupt(INTERRUPT_PIN), dmpDataReady, RISING);
  packetSize = mpu.dmpGetFIFOPacketSize();
  dmpReady = true;

  // WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nConnected!");
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();

  baselineStart = millis();
}

// ---------------- LOOP ----------------
void loop() {
  server.handleClient();

  // ----- HEART RATE -----
  long irValue = particleSensor.getIR();

  rollingAvg = 0;
  irBuffer[bufferIndex] = irValue;
  for (int i = 0; i < BUFFER_SIZE; i++) rollingAvg += irBuffer[i];
  rollingAvg /= BUFFER_SIZE;
  bufferIndex = (bufferIndex + 1) % BUFFER_SIZE;

  long acSignal = irValue - rollingAvg;

  bool currentBeat = false;
  if (irValue > FINGER_THRESHOLD && acSignal > AC_THRESHOLD) {
    if (!lastBeatState) {
      currentBeat = true;
      lastBeatState = true;
    }
  } else lastBeatState = false;

  if (currentBeat) {
    unsigned long now = millis();
    unsigned long delta = now - lastBeatTime;
    lastBeatTime = now;
    if (delta > 300 && delta < 2000) {
      bpm = 60.0 / (delta / 1000.0);
    }
  }

  // ----- GSR FILTER -----
  int rawGSR = analogRead(GSR_PIN);

  gsrSum -= gsrBuffer[gsrIndex];
  gsrBuffer[gsrIndex] = rawGSR;
  gsrSum += rawGSR;
  gsrIndex = (gsrIndex + 1) % GSR_BUFFER;

  filteredGSR = gsrSum / GSR_BUFFER;

  // ----- BASELINE -----
  if (!baselineReady) {
    baselineSum += filteredGSR;
    baselineCount++;
    if (millis() - baselineStart > 10000) {
      gsrBaseline = baselineSum / baselineCount;
      baselineReady = true;
    }
    return;
  }

  // ----- ANXIETY -----
  float deviation = abs(filteredGSR - gsrBaseline);
  float maxDev = 300.0;
  anxietyPercent = constrain((deviation / maxDev) * 100.0, 0, 100);

  // ----- MPU6050 -----
  if (dmpReady && mpu.dmpGetCurrentFIFOPacket(fifoBuffer)) {
    mpu.dmpGetQuaternion(&q, fifoBuffer);
    mpu.dmpGetGravity(&gravity, &q);
    mpu.dmpGetYawPitchRoll(ypr, &q, &gravity);
  }

  // ----- OLED -----
  display.clearDisplay();
  display.setCursor(0,0);
  display.print("BPM: "); display.println(bpm,1);
  display.print("GSR: "); display.println(filteredGSR);
  display.print("Anx: "); display.print(anxietyPercent,1); display.println("%");

  display.print("Yaw: "); display.println(ypr[0]*180/M_PI,1);
  display.print("Pitch: "); display.println(ypr[1]*180/M_PI,1);
  display.print("Roll: "); display.println(ypr[2]*180/M_PI,1);

  display.display();

  delay(SAMPLE_INTERVAL_MS);
}