#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <XPowersLib.h> 

// Knihovny pro E-ink
#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>

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

// Hlavní funkce pro kreslení na E-ink displej
void drawEinkData(bool hasFix, String lat, String lon, int sats, float course) {
  bool batConnected = PMU.isBatteryConnect();
  int batPercent = -1;

  if (batConnected) {
    uint16_t volt = PMU.getBattVoltage(); // Skutečné napětí v mV z ADC
    batPercent = PMU.getBatteryPercent(); // Procenta z fuel gauge čipu
    
    // Pokud fuel gauge ještě není zkalibrovaný nebo vrací nesmyslnou hodnotu, vypočítáme procenta přímo z napětí Li-Pol (3300 mV = 0 %, 4200 mV = 100 %)
    if ((batPercent <= 0 || batPercent > 100) && volt > 2500) {
      batPercent = map(constrain(volt, 3300, 4200), 3300, 4200, 0, 100);
    }
    batPercent = constrain(batPercent, 0, 100);
  }
  
  display.setRotation(1); // Otočení na šířku
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    
    // ==========================================
    // 1. HORNÍ LIŠTA: Signál GPS (vlevo nahoře)
    // ==========================================
    int bars = 0;
    if (sats >= 3) bars = 1;
    if (sats >= 5) bars = 2;
    if (sats >= 7) bars = 3;
    if (sats >= 9) bars = 4;

    for (int i = 0; i < 4; i++) {
      int h = (i + 1) * 3;
      int x = 5 + (i * 6);
      int y = 16 - h;
      if (i < bars) display.fillRect(x, y, 4, h, GxEPD_BLACK);
      else          display.drawRect(x, y, 4, h, GxEPD_BLACK);
    }
    
    display.setFont(); // Malý systémový font
    display.setCursor(35, 8);
    display.print(String(sats) + " Sat");

    // ==========================================
    // 2. HORNÍ LIŠTA: Baterie (vpravo nahoře)
    // ==========================================
    int bw = 24, bh = 10;
    int bx = display.width() - bw - 35; 
    int by = 5;
    
    display.drawRect(bx, by, bw, bh, GxEPD_BLACK);
    display.fillRect(bx + bw, by + 2, 2, 6, GxEPD_BLACK);
    
    if (batConnected && batPercent >= 0) {
      int fillW = map(batPercent, 0, 100, 0, bw - 2);
      if (fillW > 0) {
        display.fillRect(bx + 1, by + 1, fillW, bh - 2, GxEPD_BLACK);
      }
      display.setCursor(bx + bw + 5, by + 1);
      display.print(String(batPercent) + "%");
    } else {
      // Baterie není připojená (běží jen z USB)
      display.drawLine(bx + 2, by + 2, bx + bw - 3, by + bh - 3, GxEPD_BLACK);
      display.setCursor(bx + bw + 5, by + 1);
      display.print("--%");
    }

    // Oddělovací čára
    display.drawLine(0, 20, display.width(), 20, GxEPD_BLACK);

    // ==========================================
    // 3. STŘED: Skutečné souřadnice ze satelitu
    // ==========================================
    display.setFont(&FreeSans9pt7b);
    
    if (hasFix) {
      display.setCursor(10, 55);
      display.print("Lat:  " + lat);
      display.setCursor(10, 80);
      display.print("Lon: " + lon);
    } else {
      display.setCursor(10, 65);
      display.print("Hledam satelity...");
    }

    // ==========================================
    // 4. VPRAVO DOLE: Kompas
    // ==========================================
    String compassText = getCompassDirection(course);
    String fullCompassStr = "Smer: " + compassText;
    if (course >= 0) fullCompassStr += " (" + String((int)course) + ")";
    
    int16_t tbx, tby; uint16_t tbw, tbh;
    display.getTextBounds(fullCompassStr, 0, 0, &tbx, &tby, &tbw, &tbh);
    
    display.setCursor(display.width() - tbw - 5, display.height() - 5);
    display.print(fullCompassStr);

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

  // Počet satelitů (index 14 = viditelné, index 15 = použité)
  int sats = values[14].toInt();
  if (sats == 0 && values[15].length() > 0) {
    sats = values[15].toInt();
  }

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

    // Pokud jsme právě získali první fix, nebo uplynulo 30 s od posledního překreslení
    if (!mamRealnyFix || (millis() - lastDrawTime >= 30000) || prvniVykresleni) {
      mamRealnyFix = true;
      prvniVykresleni = false;
      posledniSats = sats;
      lastDrawTime = millis();
      drawEinkData(true, lat, lon, sats, course);
    }
  } else {
    mamRealnyFix = false;
    // Dokud čekáme na satelity, překreslíme stav okamžitě při změně počtu satelitů (min. rozestup 10 s kvůli E-inku) nebo každých 30 s
    bool zmenaSatelitu = (sats != posledniSats) && (millis() - lastDrawTime >= 10000);
    if (zmenaSatelitu || (millis() - lastDrawTime >= 30000) || prvniVykresleni) {
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
  PMU.setDC3Voltage(3400);  // Doporučené napětí pro SIM7080G (3.4V pro stabilní příjem)
  PMU.enableDC3();          // Hlavní napájení modemu
  PMU.setBLDO1Voltage(3300);
  PMU.enableBLDO1();        // Napájení převodníků komunikace
  PMU.setBLDO2Voltage(3300);
  PMU.enableBLDO2();        // Napájení pro GPS
  PMU.disableTSPinMeasure(); 
  PMU.enableBattDetection();      // Zapnutí hardwarové detekce připojené baterie
  PMU.enableBattVoltageMeasure(); // Zapnutí ADC měření skutečného napětí baterie
  delay(300);

  // 2. Inicializace E-ink displeje a zobrazení výchozího čekání
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(0, true, 2, false); 
  drawEinkData(false, "", "", 0, -1.0);
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
    
    // Počkáme, až modem opravdu naběhne a začne odpovídat na AT příkazy
    int pokusy = 0;
    while (!jeModemZapnuty() && pokusy < 15) {
      delay(500);
      pokusy++;
    }
  }

  // 4. Rychlá konfigurace GPS bez SIM karty
  sendAT("ATE0", 1000);                // Vypnutí echa pro čistší komunikaci
  sendAT("AT+CFUN=0,0", 5000);         // Okamžité vypnutí mobilního rádia (nehledá SIM ani mobilní síť)
  sendAT("AT+CGNSPWR=0", 2000);        // Vypnutí GNSS před změnou konfigurace konstelací
  sendAT("AT+SGPIO=0,5,1,1", 2000);    // Zapnutí napájení aktivní GPS antény (LNA)
  // Zapnutí GPS + GLONASS + BeiDou + Galileo (4 parametry dle specifikace SIM7080G)
  if (!sendAT("AT+CGNSMOD=1,1,1,1", 2000)) {
    sendAT("AT+CGNSMOD=1,1,1,1,1", 2000); // Záložní varianta pro jinou revizi firmwaru
  }
  sendAT("AT+CGNSPWR=1", 2000);        // Zapnutí GNSS enginu
  sendAT("AT+CGNSHOT", 2000);          // Vynucení Hot Startu (využije uložené efemeridy v RTC modemu)
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

  // Dotaz na modem: každé 2 vteřiny při hledání satelitů, každých 5 vteřin po zaměření
  unsigned long queryInterval = mamRealnyFix ? 5000 : 2000;
  if (millis() - lastQueryTime >= queryInterval) {
    lastQueryTime = millis();
    Serial1.println("AT+CGNSINF");
  }
}
