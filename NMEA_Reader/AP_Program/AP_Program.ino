/* * ==============================================================================
 * STMP26 SENSATE GPS NMEA Reader - FINAL WORKING LAB REFERENCE
 * Target Hardware: TenStar ESP32-S3 + Waveshare LC29H GPS Module
 * Recommended Software Environment: Arduino IDE 2.x.x
 * ==============================================================================
 * * 🛠️ ENGINEERING RULES OF THE ROAD (READ BEFORE FLASHING):
 * * 1. THE ENVIRONMENT VARIABILITY FACTOR: 
 * Depending on whether you are running Windows (and if it's a personal laptop 
 * or a locked-down institution machine), a Mac, or Linux, the Arduino IDE 2.x 
 * environment may interact differently with your hardware. If your port 
 * disappears or throws an "Access Denied" error, check your OS device drivers.
 * * 2. THE FIRST LAW OF ENGINEERING SKEPTICISM:
 * As an engineer, you must assume absolutely nothing works until YOU have personally 
 * tested it and verified the data stream. It does not matter if your lecturer 
 * swears it works, it does not matter if a generative AI confidently tells you 
 * it works, and it definitely does not matter if your lab partner says "it worked 
 * on my machine five minutes ago." Trust your oscilloscope, trust your serial 
 * monitor, and verify everything!
 * * ==============================================================================
 */

// ── Libraries ────────────────────────────────────────────────────────────────
#include <TinyGPSPlus.h>          // NMEA 0183 sentence parser (handles $GN prefixes)
#include <Adafruit_GFX.h>         // Core graphics primitives (drawLine, fillRect, etc.)
#include <Adafruit_ST7789.h>      // Hardware driver for the ST7789 TFT on the TenStar
#include <Adafruit_NeoPixel.h>    // Driver for the single onboard WS2812 NeoPixel
#include <SPI.h>                  // SPI bus used by the TFT display

// ── Pin Definitions (confirmed working — do not change) ─────────────────────
#define GPS_RX        17
#define GPS_TX        18
#define TFT_CS         7
#define TFT_DC        39
#define TFT_RST       40
#define TFT_BACKLIGHT 45
#define SPI_SCK       36
#define SPI_MISO      37
#define SPI_MOSI      35
#define LED_PIN       33          // Onboard NeoPixel data pin
#define NUM_PIXELS     1          // Single NeoPixel element
#include <WiFi.h>
#include <WebServer.h>

// Dynamic target coordinates (will be updated via Wi-Fi)
double TARGET_LAT = 0.0;
double TARGET_LON = 0.0;
String currentDestinationName = "Waiting for destination...";

// Rescue Wi-Fi network configuration
const char* ssid = "Sensatex_Emergency";
const char* password = ""; // No password for immediate access

WebServer server(80); // Web server on the default HTTP port

// Here we embed the entire Web App graphical interface (HTML + CSS + JavaScript)
// We use R"rawliteral(...)rawliteral" to write HTML in C++ conveniently
const char* html_page = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Sensatex Rescue</title>
    <style>
        body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: #121212; color: white; text-align: center; padding: 20px; margin: 0; }
        h1 { color: #ff4d4d; font-size: 28px; }
        .btn { display: block; width: 100%; padding: 20px; margin: 15px 0; font-size: 20px; font-weight: bold; border: none; border-radius: 10px; cursor: pointer; box-shadow: 0 4px 6px rgba(0,0,0,0.3); }
        .btn-hosp { background-color: #d9534f; color: white; }
        .btn-pol { background-color: #0275d8; color: white; }
        .btn-emb { background-color: #f0ad4e; color: white; }
        #status { margin-top: 20px; color: #5cb85c; font-weight: bold; }
    </style>
</head>
<body>
    <h1>📍 Sensatex Rescue</h1>
    <p>Select your safe zone to start off-grid navigation:</p>
    
    <button class="btn btn-hosp" onclick="sendDestination('1')">🏥 TALLAGHT HOSPITAL</button>
    <button class="btn btn-pol" onclick="sendDestination('2')">🚓 GARDA STATION</button>
    <button class="btn btn-emb" onclick="fetch('/set?dest=3')">🏛️ EMBASSY</button>

    <p id="status">Waiting for orders...</p>

    <script>
        // Function that sends the command to the ESP32 without reloading the page
        function sendDestination(number) {
            document.getElementById('status').innerText = 'Sending coordinates to radar...';
            var xhr = new XMLHttpRequest();
            xhr.open('GET', '/set?dest=' + number, true);
            xhr.onload = function() {
                if(xhr.status == 200) {
                    document.getElementById('status').innerText = '✅ Radar pointing to destination';
                }
            };
            xhr.send();
        }
    </script>
</body>
</html>
)rawliteral";

// --- WEB SERVER ROUTES ---

// What to do when the phone connects and navigates to the IP
void handleRoot() {
  server.send(200, "text/html", html_page);
}

// What to do when the phone presses a button
void handleSetDest() {
  if (server.hasArg("dest")) {
    String dest = server.arg("dest");
    
    if (dest == "1") {
      TARGET_LAT = 53.2895; TARGET_LON = -6.3768; // Hospital
      currentDestinationName = "Hospital";
    } else if (dest == "2") {
      TARGET_LAT = 53.2875; TARGET_LON = -6.3712; // Garda
      currentDestinationName = "Garda Station";
    } else if (dest == "3") {
      TARGET_LAT = 53.3283; TARGET_LON = -6.2305; // Embassy
      currentDestinationName = "Embassy";
    }
    
    Serial.print("New destination received via Wi-Fi: ");
    Serial.println(currentDestinationName);
    
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing dest parameter");
  }
}

void initSoftAP() {
  Serial.println("\n--- Starting Sensatex Wi-Fi Off-Grid ---");
  WiFi.softAP(ssid, password);
  
  IPAddress IP = WiFi.softAPIP();
  Serial.print("Command Centre IP Address: ");
  Serial.println(IP); // Usually prints 192.168.4.1

  // Configure the routes
  server.on("/", handleRoot);
  server.on("/set", handleSetDest);
  
  server.begin();
  Serial.println("Web Server activated. Searching for phones!");
}
// ── Navigation Target ───────────────────────────────────────────────────────
const double RADIUS     = 20.0;    // Destination "arrived" radius in metres

// ── Object Instances ────────────────────────────────────────────────────────
// HardwareSerial(1) = UART1 on the ESP32-S3, mapped to GPS_RX/GPS_TX below.
HardwareSerial GPSSerial(1);

// TinyGPS++ parser object — we feed it raw bytes and it extracts lat/lon/time.
TinyGPSPlus gps;

// Construct the TFT display using explicit SPI pins so the compiler never
// falls back to wrong defaults on the S3 variant.
// Use the default SPI bus and remap its pins in setup() — this is the
// most reliable method on ESP32-S3 (avoids HSPI/VSPI mapping issues).
Adafruit_ST7789 tft = Adafruit_ST7789(&SPI, TFT_CS, TFT_DC, TFT_RST);

// NeoPixel strip (1 LED) on the onboard data pin.
Adafruit_NeoPixel pixel(NUM_PIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);

// ── Timing ──────────────────────────────────────────────────────────────────
// Non-blocking refresh: we only redraw the screen once per second to prevent
// flicker while still reading GPS bytes every pass through loop().
unsigned long lastDisplayUpdate = 0;
const unsigned long DISPLAY_INTERVAL = 1000; // milliseconds

// ── Navigation State (updated every GPS fix, read by the display timer) ─────
// These globals decouple data capture from screen refresh so that a fix is
// never lost due to the display timer not having elapsed yet.
bool    hasFix      = false;
double  navDistance = 0.0;
double  navBearing  = 0.0;   // absolute bearing to target (0-360)
double  navHeading  = 0.0;   // current direction of travel from GPS (0-360)

// Heading hold is now infinite. We rely on the last known heading forever.
unsigned long headingLastValidMs = 0;       // timestamp of last good heading

// ── Forward Declarations ────────────────────────────────────────────────────
void drawArrow(int cx, int cy, int len, double angleDeg, uint16_t color);
void showArrived();
void showNavigation(double distance, double bearing);
void drawDashboard();

// ═════════════════════════════════════════════════════════════════════════════
//  SETUP — runs once after power-on or reset
// ═════════════════════════════════════════════════════════════════════════════
void setup() {
  initSoftAP();
  // --- USB Serial (debug / Serial Monitor) ---
  Serial.begin(115200);
  delay(10);

  Serial.println("========================================");
  Serial.println("STMP26 GPS NMEA Reader");
  Serial.println("TenStar ESP32-S3 + Waveshare LC29H GPS");
  Serial.println("========================================");
  Serial.println();
  Serial.println("Configuration:");
  Serial.println("  GPS Baud: 115200");
  Serial.println("  RX: GPIO17");
  Serial.println("  TX: GPIO18");
  Serial.println();
  Serial.println("Waiting for GPS data...\n");

  // --- GPS UART ---
  // 115200 baud is the LC29H default (NOT the common 9600 of older modules).
  GPSSerial.begin(115200, SERIAL_8N1, GPS_RX, GPS_TX);
  delay(10);

  // --- NeoPixel ---
  pixel.begin();
  pixel.setBrightness(40);        // Keep brightness modest to avoid glare
  pixel.setPixelColor(0, pixel.Color(0, 0, 50));  // Dim blue = "booting"
  pixel.show();

  // --- TFT Display ---
  // IMPORTANT: power the backlight BEFORE calling tft.init() or the screen
  // stays black — this is a hardware quirk of the TenStar board.
  pinMode(TFT_BACKLIGHT, OUTPUT);
  digitalWrite(TFT_BACKLIGHT, HIGH);

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  tft.init(135, 240);             // 135×240 native pixel resolution
  tft.setRotation(3);             // Landscape mode (wide = 240, tall = 135)
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(4, 4);
  tft.println("Waiting for GPS fix...");

  delay(10);
}

// ═════════════════════════════════════════════════════════════════════════════
//  LOOP — runs continuously
// ═════════════════════════════════════════════════════════════════════════════
void loop() {
  server.handleClient();
  // --- 1. Feed every available byte to TinyGPS++ (non-blocking) ---
  while (GPSSerial.available()) {
    gps.encode(GPSSerial.read());
  }

  // --- 2. Capture navigation data whenever a new fix arrives ───────────
  // isUpdated() returns true ONCE per new fix, then resets to false.
  // We store the results in globals immediately so nothing is lost.
  if (gps.location.isUpdated() && gps.location.isValid()) {
    hasFix = true;

    double currLat = gps.location.lat();
    double currLon = gps.location.lng();

    // Great-circle distance in metres from current position to target.
    navDistance = TinyGPSPlus::distanceBetween(
                    currLat, currLon, TARGET_LAT, TARGET_LON);

    // Initial bearing (0–360°) from current position to target.
    navBearing = TinyGPSPlus::courseTo(
                   currLat, currLon, TARGET_LAT, TARGET_LON);

    // Current direction of travel reported by the GPS receiver.
    // NOTE: course is only valid when you are physically MOVING.
    // If we are stationary, the GPS outputs random noise for heading.
    // We strictly ONLY update navHeading if we are walking (> 0.8 km/h).
    if (gps.course.isValid() && gps.speed.isValid() && gps.speed.kmph() > 0.8) {
      navHeading = gps.course.deg();
    }
  }

  // Check if we lost the fix (no new location data for over 2 seconds)
  if (hasFix && gps.location.age() > 2000) {
    hasFix = false;
  }

  // --- 3. Refresh the display once per second ─────
  // This runs on its own timer so the screen always redraws with the
  // latest stored values, or shows a warning if the fix is lost.
  if (millis() - lastDisplayUpdate >= DISPLAY_INTERVAL) {
    lastDisplayUpdate = millis();

    if (hasFix) {
      // Debug: print values to Serial Monitor so you can verify data is live.
      Serial.print("[NAV] dist=");
      Serial.print(navDistance, 1);
      Serial.print("m  absBrg=");
      Serial.print(navBearing, 1);
      Serial.print("°  heading=");
      Serial.print(navHeading, 1);
      Serial.print("°  relBrg=");
      double rel = navBearing - navHeading;
      if (rel < 0) rel += 360.0;
      if (rel >= 360.0) rel -= 360.0;
      Serial.print(rel, 1);
      Serial.println("°");

      if (navDistance <= RADIUS) {
        showArrived();
      } else {
        showNavigation(navDistance, navBearing);
      }

      // Also print the full PVT dashboard to the TFT
      drawDashboard();
    } else {
      // We do not have a fix, or we lost it
      tft.fillScreen(ST77XX_BLACK);
      tft.setTextColor(ST77XX_WHITE);
      tft.setTextSize(1);
      tft.setCursor(4, 4);
      tft.println(gps.location.age() > 2000 ? "Lost GPS fix, waiting..." : "Waiting for GPS fix...");
      
      // Set NeoPixel back to dim blue
      pixel.setPixelColor(0, pixel.Color(0, 0, 50));
      pixel.show();
    }
  }

  // --- 3. Echo raw NMEA sentences to Serial Monitor for debugging ---
  // (This section is intentionally left as a secondary path so that the
  //  Serial Monitor remains a live diagnostic window.)
  // The raw bytes were already consumed by gps.encode() above, so we use
  // TinyGPS++ statistics instead of re-reading Serial1.
}

// ═════════════════════════════════════════════════════════════════════════════
//  showArrived — full-screen green celebration when within RADIUS
// ═════════════════════════════════════════════════════════════════════════════
void showArrived() {
  // Fill the entire screen with a bright green background.
  tft.fillScreen(ST77XX_GREEN);

  // Print the arrival message in large, centred text.
  tft.setTextColor(ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(20, 30);
  tft.println("ARRIVED TO");
  tft.setCursor(20, 55);
  tft.println("DESTINATION!");

  // Show distance = 0 for clarity.
  tft.setTextSize(1);
  tft.setCursor(20, 90);
  tft.print("Dist: < ");
  tft.print(RADIUS, 1);
  tft.println(" m");

  // Light the NeoPixel solid green.
  pixel.setPixelColor(0, pixel.Color(0, 255, 0));
  pixel.show();
}

// ═════════════════════════════════════════════════════════════════════════════
//  showNavigation — distance + directional arrow while en route
// ═════════════════════════════════════════════════════════════════════════════
void showNavigation(double distance, double bearing) {
  // Clear previous frame.
  tft.fillScreen(ST77XX_BLACK);

  // ── Always use relative bearing ────────────────────────────────────────
  // GPS heading (course) is derived from movement between fixes.
  // We will permanently hold the last known heading so the arrow ALWAYS
  // acts as a relative turn indicator based on the last direction of travel.
  
  const double SPEED_THRESHOLD = 0.8;  // km/h — a gentle walk
  bool movingNow = gps.speed.isValid() && (gps.speed.kmph() > SPEED_THRESHOLD);

  if (movingNow) {
    headingLastValidMs = millis();
  }

  double arrowAngle = bearing - navHeading;
  if (arrowAngle < 0)    arrowAngle += 360.0;
  if (arrowAngle >= 360) arrowAngle -= 360.0;
  
  const char* modeLabel = "REL";

  // ── Top section: distance remaining ────────────────────────────────────
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(1);
  tft.setCursor(4, 4);
  tft.print("Dist: ");

  if (distance >= 1000.0) {
    tft.print(distance / 1000.0, 2);
    tft.println(" km");
  } else {
    tft.print(distance, 1);
    tft.println(" m");
  }

  tft.setCursor(4, 16);
  tft.print("Brg:");
  tft.print(bearing, 0);
  tft.print((char)247);           // ° symbol
  if (movingNow) {
    tft.print(" Hdg:");
    tft.print(navHeading, 0);
    tft.print((char)247);
  }
  tft.print(" [");
  tft.print(modeLabel);
  tft.print("]");

  // ── Centre section: directional arrow ──────────────────────────────────
  // Uses relative bearing when moving (turn indicator) or absolute bearing
  // when still (compass pointer toward target).
  drawArrow(120, 65, 35, arrowAngle, ST77XX_YELLOW);

  // Set NeoPixel to orange while navigating (not yet arrived).
  pixel.setPixelColor(0, pixel.Color(255, 80, 0));
  pixel.show();
}

// ═════════════════════════════════════════════════════════════════════════════
//  drawArrow — renders a bold, filled directional arrow on the TFT
//
//  cx, cy  : centre point of the arrow
//  len     : distance from centre to the tip
//  angleDeg: compass bearing in degrees (0° = North = screen-up)
//  color   : 16-bit 565 colour
//
//  Anatomy of the arrow (drawn along the bearing axis):
//
//        ▲  tip                   ← filled triangle (arrowhead)
//       ╱ ╲
//      ╱   ╲  ← headWidth on each side
//     ╱_____╲ base of head
//       | |
//       | |   ← thick shaft (multiple parallel lines)
//       |_|
//       tail
// ═════════════════════════════════════════════════════════════════════════════
void drawArrow(int cx, int cy, int len, double angleDeg, uint16_t color) {
  // Convert compass bearing to a screen-space angle in radians.
  // Compass: 0° = North (up), increases clockwise.
  // Screen:  +x = right, +y = DOWN.
  // Using sin/cos directly:  dx = sin(bearing), dy = -cos(bearing).
  double rad = angleDeg * PI / 180.0;
  double dx  =  sin(rad);        // unit vector component along +x
  double dy  = -cos(rad);        // unit vector component along +y (screen)

  // Perpendicular unit vector (90° clockwise on screen) for width offsets.
  double px = -dy;               //  cos(bearing)
  double py =  dx;               //  sin(bearing)

  // ── Key points ────────────────────────────────────────────────────────
  // Tip: the foremost point of the arrowhead.
  int tipX = cx + (int)(len * dx);
  int tipY = cy + (int)(len * dy);

  // Tail: the rearmost point of the shaft.
  int tailX = cx - (int)(len * dx);
  int tailY = cy - (int)(len * dy);

  // Base of the arrowhead (where the triangular head meets the shaft).
  // The head occupies 45 % of the total length.
  double headFrac = 0.45;
  int baseX = cx + (int)(len * (1.0 - headFrac * 2.0) * dx);
  int baseY = cy + (int)(len * (1.0 - headFrac * 2.0) * dy);

  // ── Arrowhead (filled triangle) ───────────────────────────────────────
  int headHalfW = (int)(len * 0.38);   // half-width of the head base
  int lx = baseX + (int)(headHalfW * px);
  int ly = baseY + (int)(headHalfW * py);
  int rx = baseX - (int)(headHalfW * px);
  int ry = baseY - (int)(headHalfW * py);

  tft.fillTriangle(tipX, tipY, lx, ly, rx, ry, color);

  // ── Shaft (thick, 5-pixel wide band of parallel lines) ────────────────
  int shaftHalfW = 3;
  for (int i = -shaftHalfW; i <= shaftHalfW; i++) {
    int ox = (int)(i * px);
    int oy = (int)(i * py);
    tft.drawLine(tailX + ox, tailY + oy, baseX + ox, baseY + oy, color);
  }
}

// ═════════════════════════════════════════════════════════════════════════════
//  drawDashboard — PVT telemetry overlay (bottom of screen)
//  Displays Position, Velocity, Time, Satellites, HDOP, Fix status.
// ═════════════════════════════════════════════════════════════════════════════
void drawDashboard() {
  // We draw in the lower portion of the screen so as not to overlap the
  // navigation arrow or arrival message painted by the caller.
  int y = 108;                    // Starting Y for the telemetry line

  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK); // White-on-black, auto-clear
  tft.setTextSize(1);

  // ── Position ───────────────────────────────────────────────────────────
  tft.setCursor(4, y);
  if (gps.location.isValid()) {
    tft.print("P:");
    tft.print(gps.location.lat(), 6);
    tft.print(",");
    tft.print(gps.location.lng(), 6);
  } else {
    tft.print("P: no fix        ");
  }

  // ── Satellites + HDOP ──────────────────────────────────────────────────
  tft.setCursor(4, y + 10);
  tft.print("Sat:");
  tft.print(gps.satellites.value());
  tft.print(" HDOP:");
  if (gps.hdop.isValid()) {
    tft.print(gps.hdop.hdop(), 1);
  } else {
    tft.print("--");
  }

  // ── Velocity ───────────────────────────────────────────────────────────
  tft.setCursor(140, y);
  tft.print("V:");
  if (gps.speed.isValid()) {
    tft.print(gps.speed.kmph(), 1);
    tft.print("km/h");
  } else {
    tft.print("--    ");
  }

  // ── Time (UTC) ─────────────────────────────────────────────────────────
  tft.setCursor(140, y + 10);
  tft.print("T:");
  if (gps.time.isValid()) {
    if (gps.time.hour() < 10)   tft.print('0');
    tft.print(gps.time.hour());   tft.print(':');
    if (gps.time.minute() < 10) tft.print('0');
    tft.print(gps.time.minute()); tft.print(':');
    if (gps.time.second() < 10) tft.print('0');
    tft.print(gps.time.second());
  } else {
    tft.print("--:--:--");
  }
}