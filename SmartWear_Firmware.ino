#include <WiFi.h>

#include <WebServer.h>

#include <Wire.h>

#include <MPU6050.h>

#include <Adafruit_GFX.h>

#include <Adafruit_SSD1306.h>

#include "MAX30105.h"

#include "heartRate.h"



// ================= OBJECTS =================

MPU6050 mpu;

MAX30105 particleSensor;

WebServer server(80);



// ================= WIFI AP =================

const char* ssid = "ESP32-HealthMonitor";

const char* password = "12345678";



// ================= OLED =================

#define SCREEN_WIDTH 128

#define SCREEN_HEIGHT 64

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);



// ================= HEART RATE =================

const byte RATE_SIZE = 10;

byte rates[RATE_SIZE];

byte rateSpot = 0;

long lastBeat = 0;

float beatsPerMinute;

int beatAvg = 0;



// ================= SPO2 =================

double avered = 0, aveir = 0;

double sumirrms = 0, sumredrms = 0;

double SpO2 = 0, ESpO2 = 60.0;

double FSpO2 = 0.7, frate = 0.95;

int i = 0, Num = 30;



#define FINGER_ON 7000

#define MINIMUM_SPO2 60.0



// ================= GSR =================

#define GSR_PIN 6

int gsrValue = 0;



// ================= PSYCHO STATE =================

String psychState = "Neutral";



// ================= HTML =================

String dashboardHTML() {

  return R"rawliteral(

<!DOCTYPE html>

<html>

<head>

<meta name="viewport" content="width=device-width, initial-scale=1">

<title>ESP32 Health Dashboard</title>

<style>

body { font-family: Arial; background:#0f172a; color:white; text-align:center; }

.card { background:#1e293b; padding:15px; margin:10px; border-radius:10px; }

h1 { color:#38bdf8; }

.state { font-size:22px; font-weight:bold; }

</style>

</head>

<body>

<h1>Health Monitoring</h1>

<div class="card">BPM: <span id="bpm">0</span></div>

<div class="card">SpO₂: <span id="spo2">0</span>%</div>

<div class="card">GSR: <span id="gsr">0</span></div>

<div class="card">State:<br><span class="state" id="state">---</span></div>



<script>

setInterval(()=>{

fetch("/data").then(r=>r.json()).then(d=>{

document.getElementById("bpm").innerText=d.bpm;

document.getElementById("spo2").innerText=d.spo2;

document.getElementById("gsr").innerText=d.gsr;

document.getElementById("state").innerText=d.state;

});

},1000);

</script>

</body>

</html>

)rawliteral";

}



// ================= PSYCHO CLASSIFIER =================

void classifyState(int bpm, int gsr, int ax, int ay, int az) {

  float motion = sqrt(ax*ax + ay*ay + az*az);



  if (gsr < 300 && bpm < 75)

    psychState = "Relaxed";

  else if (gsr < 500 && bpm < 90)

    psychState = "Neutral";

  else if (gsr < 700 || bpm < 110)

    psychState = "Stressed";

  else

    psychState = "Very Stressed";

}



// ================= SERVER DATA =================

void handleData() {

  String json = "{";

  json += "\"bpm\":" + String(beatAvg) + ",";

  json += "\"spo2\":" + String(ESpO2) + ",";

  json += "\"gsr\":" + String(gsrValue) + ",";

  json += "\"state\":\"" + psychState + "\"";

  json += "}";

  server.send(200, "application/json", json);

}



// ================= SETUP =================

void setup() {

  Serial.begin(115200);

  Serial.println("BPM,SpO2,GSR,AX,AY,AZ,GX,GY,GZ");



  Wire.begin(12, 13);



  // OLED

  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);

  display.clearDisplay();

  display.display();



  // MAX30105

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {

    while (1);

  }

  particleSensor.setup(0x7F, 4, 2, 800, 215, 16384);



  // MPU6050

  mpu.initialize();



  pinMode(GSR_PIN, INPUT);



  // WIFI AP

  WiFi.softAP(ssid, password);

  server.on("/", []() { server.send(200, "text/html", dashboardHTML()); });

  server.on("/data", handleData);

  server.begin();

}



// ================= LOOP =================

void loop() {

  server.handleClient();



  int16_t ax, ay, az, gx, gy, gz;

  mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);



  long irValue = particleSensor.getIR();



  if (irValue > FINGER_ON) {



    if (checkForBeat(irValue)) {

      long delta = millis() - lastBeat;

      lastBeat = millis();

      beatsPerMinute = 60 / (delta / 1000.0);



      if (beatsPerMinute > 20 && beatsPerMinute < 255) {

        rates[rateSpot++] = (byte)beatsPerMinute;

        rateSpot %= RATE_SIZE;

        beatAvg = 0;

        for (byte x = 0; x < RATE_SIZE; x++) beatAvg += rates[x];

        beatAvg /= RATE_SIZE;

      }

    }



    particleSensor.check();

    if (particleSensor.available()) {

      i++;

      uint32_t ir = particleSensor.getFIFOIR();

      uint32_t red = particleSensor.getFIFORed();

      double fir = ir, fred = red;



      aveir = aveir * frate + fir * (1.0 - frate);

      avered = avered * frate + fred * (1.0 - frate);

      sumirrms += (fir - aveir) * (fir - aveir);

      sumredrms += (fred - avered) * (fred - avered);



      if (i % Num == 0) {

        double R = (sqrt(sumirrms) / aveir) / (sqrt(sumredrms) / avered);

        SpO2 = -23.3 * (R - 0.4) + 120;

        ESpO2 = FSpO2 * ESpO2 + (1.0 - FSpO2) * SpO2;

        ESpO2 = constrain(ESpO2, MINIMUM_SPO2, 99.9);

        sumirrms = sumredrms = i = 0;

      }

      particleSensor.nextSample();

    }



    gsrValue = analogRead(GSR_PIN);

    classifyState(beatAvg, gsrValue, ax, ay, az);



    Serial.print(beatAvg); Serial.print(",");

    Serial.print(ESpO2); Serial.print(",");

    Serial.print(gsrValue); Serial.print(",");

    Serial.print(ax); Serial.print(",");

    Serial.print(ay); Serial.print(",");

    Serial.print(az); Serial.print(",");

    Serial.print(gx); Serial.print(",");

    Serial.print(gy); Serial.print(",");

    Serial.println(gz);



    display.clearDisplay();

    display.setTextSize(1);

    display.setCursor(0,0); display.print("BPM: "); display.print(beatAvg);

    display.setCursor(70,0); display.print("O2: "); display.print(ESpO2);

    display.setCursor(0,15); display.print("GSR: "); display.print(gsrValue);

    display.setCursor(0,30); display.print(psychState);

    display.display();

  }



  else {

    beatAvg = 0; ESpO2 = 0; gsrValue = 0;

    display.clearDisplay();

    display.setTextSize(2);

    display.setCursor(20,10); display.print("Place");

    display.setCursor(20,35); display.print("Finger");

    display.display();

  }

}