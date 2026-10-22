#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <XPowersLib.h> 

// Knihovny pro E-ink
#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <math.h>

XPowersAXP2101 PMU;

// --- PINOUT T-SIM7080G S3 ---
#define I2C_SDA               15
#define I2C_SCL               7
#define BOARD_MODEM_RXD_PIN   4
#define BOARD_MODEM_TXD_PIN   5
#define BOARD_MODEM_PWR_PIN   41

// --- PINOUT E-INK DISPLEJE ---
#define EPD_CS    10
#define EPD_DC    9
#define EPD_RST   8
#define EPD_BUSY  14  
#define EPD_MOSI  11  
#define EPD_SCK   12  

// Inicializace 2.13" E-ink displeje
GxEPD2_BW<GxEPD2_213_BN, GxEPD2_213_BN::HEIGHT> display(GxEPD2_213_BN(/*CS=*/ EPD_CS, /*DC=*/ EPD_DC, /*RST=*/ EPD_RST, /*BUSY=*/ EPD_BUSY));

unsigned long lastQueryTime = 0;
unsigned long lastDrawTime = 0;
String modemBuffer = ""; 
bool mamRealnyFix = false;
bool prvniVykresleni = true;
int posledniSats = -1;
String posledniUTC = "";

// Pomocná funkce pro spolehlivé odeslání AT příkazu a čekání na odpověď OK
bool sendAT(const char* cmd, unsigned long timeout = 2000) {
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmd);
  Serial.print(">> ");
  Serial.println(cmd);
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < timeout) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("OK") != -1) {
        Serial.println("   OK");
        return true;
      }
      if (resp.indexOf("ERROR") != -1) {
        Serial.println("   ERROR");
        return false;
      }
    }
  }
  Serial.println("   TIMEOUT");
  return false;
}

// Kontrola, zda už modem běží (aby ho nový nahrávaný kód omylem nevypnul)
bool jeModemZapnuty() {
  for (int i = 0; i < 3; i++) {
    if (sendAT("AT", 500)) return true;
    delay(100);
  }
  return false;
}

// Funkce pro převod stupňů z GPS na textový kompas
String getCompassDirection(float course) {
  if (course < 0) return "--";
  if (course >= 337.5 || course < 22.5) return "S";
  if (course >= 22.5 && course < 67.5) return "SV";
  if (course >= 67.5 && course < 112.5) return "V";
  if (course >= 112.5 && course < 157.5) return "JV";
  if (course >= 157.5 && course < 202.5) return "J";
  if (course >= 202.5 && course < 247.5) return "JZ";
  if (course >= 247.5 && course < 292.5) return "Z";
  if (course >= 292.5 && course < 337.5) return "SZ";
  return "--";
}

// Hlavní funkce pro kreslení na E-ink displej (Moderní Dashboard UI)
void drawEinkData(bool hasFix, String lat, String lon, int sats, float course) {
  bool batConnected = PMU.isBatteryConnect();
  bool isCharging = PMU.isCharging();
  int batPercent = -1;
  uint16_t batVolt = 0;

  if (batConnected) {
    batVolt = PMU.getBattVoltage();       // Skutečné napětí v mV z ADC
    batPercent = PMU.getBatteryPercent(); // Procenta z fuel gauge čipu
    
    // Záložní výpočet z napětí Li-Pol (3300 mV = 0 %, 4200 mV = 100 %)
    if ((batPercent <= 0 || batPercent > 100) && batVolt > 2500) {
      batPercent = map(constrain(batVolt, 3300, 4200), 3300, 4200, 0, 100);
    }
    batPercent = constrain(batPercent, 0, 100);
  }
  
  display.setRotation(1); // 250 x 122 px na šířku
  // První vykreslení udělá plný refresh, další překreslení rychlé částečné okno bez nepříjemného černobílého blikání
  if (prvniVykresleni) {
    display.setFullWindow();
  } else {
    display.setPartialWindow(0, 0, display.width(), display.height());
  }

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    
    // =========================================================
    // 1. HORNÍ TMAVÁ STAVOVÁ LIŠTA (250 x 22 px)
    // =========================================================
    display.fillRect(0, 0, display.width(), 22, GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    display.setFont(); // Ostrý pixelový font 6x8 do stavové lišty

    // Vlevo: Ikona satelitu / signálu + počet družic
    int bars = 0;
    if (sats >= 1) bars = 1;
    if (sats >= 3) bars = 2;
    if (sats >= 5) bars = 3;
    if (sats >= 8) bars = 4;

    for (int i = 0; i < 4; i++) {
      int h = (i + 1) * 3 + 2;
      int x = 6 + (i * 5);
      int y = 18 - h;
      if (i < bars) display.fillRect(x, y, 3, h, GxEPD_WHITE);
      else          display.drawRect(x, y, 3, h, GxEPD_WHITE);
    }
    display.setCursor(29, 7);
    display.print(String(sats) + " SAT");

    // Uprostřed: Status pill (LIVE FIX vs SEARCHING)
    String statusBadge = hasFix ? "GPS FIX" : "HLEDAM";
    int badgeW = hasFix ? 52 : 46;
    int badgeX = (display.width() - badgeW) / 2;
    if (hasFix) {
      display.fillRoundRect(badgeX, 3, badgeW, 16, 4, GxEPD_WHITE);
      display.setTextColor(GxEPD_BLACK);
      display.setCursor(badgeX + 5, 7);
      display.print(statusBadge);
      display.setTextColor(GxEPD_WHITE);
    } else {
      display.drawRoundRect(badgeX, 3, badgeW, 16, 4, GxEPD_WHITE);
      display.setCursor(badgeX + 5, 7);
      display.print(statusBadge);
    }

    // Vpravo: Moderní ikona baterie + reálná procenta
    int bw = 22, bh = 11;
    int bx = display.width() - bw - 34; 
    int by = 5;
    display.drawRoundRect(bx, by, bw, bh, 2, GxEPD_WHITE);
    display.fillRect(bx + bw, by + 3, 2, 5, GxEPD_WHITE);
    
    if (batConnected && batPercent >= 0) {
      int fillW = map(batPercent, 0, 100, 0, bw - 4);
      if (fillW > 0) {
        display.fillRect(bx + 2, by + 2, fillW, bh - 4, GxEPD_WHITE);
      }
      display.setCursor(bx + bw + 5, 7);
      display.print(String(batPercent) + (isCharging ? "+" : "%"));
    } else {
      display.drawLine(bx + 3, by + 2, bx + bw - 4, by + bh - 3, GxEPD_WHITE);
      display.setCursor(bx + bw + 5, 7);
      display.print("USB");
    }

    display.setTextColor(GxEPD_BLACK);

    // =========================================================
    // 2. HLAVNÍ KARTA SOUŘADNIC (Vlevo: 6 až 168 px)
    // =========================================================
    display.drawRoundRect(5, 27, 164, 72, 5, GxEPD_BLACK);

    if (hasFix) {
      // Štítky LAT a LON v inverzních kapslích
      display.fillRoundRect(11, 35, 28, 15, 3, GxEPD_BLACK);
      display.setTextColor(GxEPD_WHITE);
      display.setFont();
      display.setCursor(16, 39);
      display.print("LAT");

      display.fillRoundRect(11, 66, 28, 15, 3, GxEPD_BLACK);
      display.setCursor(16, 70);
      display.print("LON");

      // Hodnoty souřadnic tučným moderním fontem
      display.setTextColor(GxEPD_BLACK);
      display.setFont(&FreeMonoBold9pt7b);
      display.setCursor(45, 48);
      display.print(lat.substring(0, 9));
      display.setCursor(45, 79);
      display.print(lon.substring(0, 9));

      // Jemná dělicí linka mezi LAT a LON
      display.drawLine(11, 58, 162, 58, GxEPD_BLACK);
    } else {
      // Radarová ikona hledání satelitů vlevo v kartě
      int rx = 28, ry = 56;
      display.drawCircle(rx, ry, 14, GxEPD_BLACK);
      display.drawCircle(rx, ry, 8, GxEPD_BLACK);
      display.fillCircle(rx, ry, 2, GxEPD_BLACK);
      display.drawLine(rx, ry, rx + 10, ry - 10, GxEPD_BLACK);

      display.setFont(&FreeSansBold9pt7b);
      display.setCursor(50, 52);
      display.print("Hledam GPS");

      display.setFont();
      display.setCursor(50, 62);
      display.print("Cekam na signal...");

      // Progress / status lišta ve spodní části karty
      display.drawRoundRect(12, 80, 150, 12, 3, GxEPD_BLACK);
      int animW = ((millis() / 1000) % 10) * 14 + 10;
      if (animW > 146) animW = 146;
      display.fillRoundRect(14, 82, animW, 8, 2, GxEPD_BLACK);
    }

    // =========================================================
    // 3. KRUHOVÝ KOMPAS (Vpravo: 174 až 245 px)
    // =========================================================
    display.drawRoundRect(174, 27, 71, 72, 5, GxEPD_BLACK);
    int cx = 209, cy = 56, cr = 20;
    display.drawCircle(cx, cy, cr, GxEPD_BLACK);
    display.drawCircle(cx, cy, cr - 1, GxEPD_BLACK);

    // Značky světových stran na ciferníku
    display.setFont();
    display.setCursor(cx - 2, cy - cr + 3);
    display.print("S");
    display.drawPixel(cx + cr - 2, cy, GxEPD_BLACK);
    display.drawPixel(cx - cr + 2, cy, GxEPD_BLACK);
    display.drawPixel(cx, cy + cr - 2, GxEPD_BLACK);

    // Střelka kompasu nebo středový bod
    if (course >= 0) {
      float rad = (course - 90.0) * PI / 180.0;
      int tipX = cx + (int)(cos(rad) * (cr - 5));
      int tipY = cy + (int)(sin(rad) * (cr - 5));
      display.drawLine(cx, cy, tipX, tipY, GxEPD_BLACK);
      display.fillCircle(tipX, tipY, 2, GxEPD_BLACK);
      display.fillCircle(cx, cy, 2, GxEPD_BLACK);
    } else {
      display.fillCircle(cx, cy, 2, GxEPD_BLACK);
      display.drawLine(cx - 5, cy, cx + 5, cy, GxEPD_BLACK);
    }

    // Text směru pod kompasem
    String compassText = getCompassDirection(course);
    if (course >= 0) compassText += " " + String((int)course) + "st";
    else             compassText = "SMER: --";
    int16_t tbx, tby; uint16_t tbw, tbh;
    display.getTextBounds(compassText, 0, 0, &tbx, &tby, &tbw, &tbh);
    display.setCursor(174 + (71 - tbw) / 2, 87);
    display.print(compassText);

    // =========================================================
    // 4. SPODNÍ INFORMAČNÍ PATIČKA (103 až 122 px)
    // =========================================================
    display.drawLine(5, 104, display.width() - 6, 104, GxEPD_BLACK);
    display.setFont();
    
    // Vlevo dole: Čas ze satelitu (nebo stav synchronizace)
    display.setCursor(6, 110);
    if (posledniUTC.length() >= 14) {
      String hh = posledniUTC.substring(8, 10);
      String mm = posledniUTC.substring(10, 12);
      String ss = posledniUTC.substring(12, 14);
      display.print("UTC " + hh + ":" + mm + ":" + ss);
    } else {
      display.print("UTC --:--:--");
    }

    // Uprostřed dole: Napětí baterie v mV / V
    if (batConnected && batVolt > 0) {
      display.setCursor(98, 110);
      display.print(String(batVolt / 1000.0, 2) + "V");
    }

    // Vpravo dole: Čas běhu (Uptime)
    unsigned long sec = millis() / 1000;
    String uptimeStr = "BEH " + String(sec / 60) + "m" + String(sec % 60) + "s";
    display.getTextBounds(uptimeStr, 0, 0, &tbx, &tby, &tbw, &tbh);
    display.setCursor(display.width() - tbw - 6, 110);
    display.print(uptimeStr);

  } while (display.nextPage());
}

// Zpracování surových dat ze satelitního modemu
void parseGPS(String data) {
  data.replace("+CGNSINF: ", "");
  
  String values[25]; 
  int index = 0;
  int startIndex = 0;
  int commaIndex = data.indexOf(',');
  
  while(commaIndex != -1 && index < 24) {
    values[index++] = data.substring(startIndex, commaIndex);
    startIndex = commaIndex + 1;
    commaIndex = data.indexOf(',', startIndex);
  }
  values[index] = data.substring(startIndex); 

  // Uložení případného času ze satelitu (index 2)
  posledniUTC = values[2];

  // Počet satelitů (index 14 = viditelné satelity, index 15 = použité pro fix)
  int gpsView = values[14].toInt();
  int gnssUsed = values[15].toInt();
  int sats = max(gpsView, gnssUsed);
  // Pojistka proti vyčtení HPA (9999000.0), pokud má firmware méně polí
  if (sats > 100) sats = 0;

  // Přísná kontrola, že máme opravdu platný satelitní fix a reálná čísla souřadnic
  bool validFix = (values[1] == "1") && 
                  (values[3].length() > 2) && 
                  (values[4].length() > 2) && 
                  (values[3].toFloat() != 0.0) && 
                  (values[4].toFloat() != 0.0);

  if (validFix) {
    String lat = values[3];
    String lon = values[4];
    float course = (values[7].length() > 0) ? values[7].toFloat() : -1.0;

    // Aktivní obnovování na displeji každých 5 vteřin (nebo okamžitě při prvním fixu)
    if (!mamRealnyFix || (millis() - lastDrawTime >= 5000) || prvniVykresleni) {
      mamRealnyFix = true;
      prvniVykresleni = false;
      posledniSats = sats;
      lastDrawTime = millis();
      drawEinkData(true, lat, lon, sats, course);
    }
  } else {
    mamRealnyFix = false;
    // Aktivní obnovování při hledání: okamžitě při změně počtu satelitů nebo každých 5 vteřin
    if ((sats != posledniSats) || (millis() - lastDrawTime >= 5000) || prvniVykresleni) {
      prvniVykresleni = false;
      posledniSats = sats;
      lastDrawTime = millis();
      drawEinkData(false, "", "", sats, -1.0);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // 1. Inicializace napájení (PMU)
  Wire.begin(I2C_SDA, I2C_SCL);
  PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C_SDA, I2C_SCL);

  PMU.setDC1Voltage(3300);
  PMU.enableDC1();          // Napájení pro E-ink a 3.3V periferie
  PMU.setDC3Voltage(3000);  // Přesně 3000 mV podle oficiálního příkladu LilyGO
  PMU.enableDC3();          // Hlavní napájení modemu (nevypínáme při resetu, aby si pamatoval satelity!)
  PMU.setBLDO1Voltage(3300);
  PMU.enableBLDO1();        // Napájení převodníků komunikace
  PMU.setBLDO2Voltage(3300);
  PMU.enableBLDO2();        // Napájení pro aktivní GPS anténu (LNA)
  PMU.disableTSPinMeasure(); 
  PMU.enableBattDetection();      // Zapnutí hardwarové detekce připojené baterie
  PMU.enableBattVoltageMeasure(); // Zapnutí ADC měření skutečného napětí baterie
  delay(300);

  // 2. Inicializace E-ink displeje a zobrazení výchozího čekání
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(0, true, 2, false); 
  drawEinkData(false, "", "", 0, -1.0);
  prvniVykresleni = false;
  lastDrawTime = millis();

  // 3. Spuštění sériové linky a zapnutí modemu (pokud ještě neběží)
  Serial1.begin(115200, SERIAL_8N1, BOARD_MODEM_RXD_PIN, BOARD_MODEM_TXD_PIN);
  pinMode(BOARD_MODEM_PWR_PIN, OUTPUT);

  if (!jeModemZapnuty()) {
    digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
    delay(100);
    digitalWrite(BOARD_MODEM_PWR_PIN, HIGH);
    delay(1000);
    digitalWrite(BOARD_MODEM_PWR_PIN, LOW);

    int pokusy = 0;
    while (!jeModemZapnuty() && pokusy < 15) {
      delay(500);
      pokusy++;
    }
  }

  // 4. Zapnutí aktivní antény a GPS + GLONASS ve správném pořadí
  sendAT("ATE0", 1000);
  sendAT("AT+CGNSPWR=0", 2000);        // Nejprve vypnout GNSS
  sendAT("AT+CFUN=4", 5000);           // Flight mode (vypne mobilní rádio, nechá čisté GPS)
  sendAT("AT+CGNSMOD=1,1,0,0,0", 2000);// GPS + GLONASS (vrací OK)
  sendAT("AT+SGPIO=0,5,1,1", 2000);    // Napájení aktivní antény (GPIO 5)
  sendAT("AT+SGPIO=0,4,1,1", 2000);    // Napájení aktivní antény (GPIO 4)
  sendAT("AT+CGNSPWR=1", 2000);        // Zapnout GNSS engine
}

void loop() {
  // Čtení odpovědi ze satelitního modemu
  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\n') { 
      modemBuffer.trim(); 
      if (modemBuffer.length() > 0) {
        Serial.println(modemBuffer);
      }
      if (modemBuffer.startsWith("+CGNSINF:")) {
        parseGPS(modemBuffer);
      } 
      modemBuffer = ""; 
    } else {
      modemBuffer += c; 
    }
  }

  // Aktivní dotaz na modem každou 1 vteřinu
  if (millis() - lastQueryTime >= 1000) {
    lastQueryTime = millis();
    Serial1.println("AT+CGNSINF");
  }
}
