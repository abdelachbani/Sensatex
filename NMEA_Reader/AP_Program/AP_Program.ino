/* * ==============================================================================
 * GPS NMEA Reader - FINAL WORKING LAB REFERENCE
 * Target Hardware: TenStar ESP32-S3 + Waveshare LC29H GPS Module
 * Recommended Software Environment: Arduino IDE 2.x.x
 * ==============================================================================
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
// This is the full app: compass, persistent locations, coordinate conversion
const char html_content[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
    <meta name="description" content="Sensatex Rescue — Off-grid GPS navigation with compass direction indicator. Add and manage locations with multiple coordinate systems.">
    <title>Sensatex Rescue — Off-Grid Navigation</title>
    <link rel="stylesheet" href="style.css">
    <link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><text y='.9em' font-size='90'>🧭</text></svg>">
</head>
<body>
    <div class="app">

        <!-- ── Header ──────────────────────────────────────────────── -->
        <header class="header" id="header">
            <div class="header__logo">
                <div class="header__icon">🛰️</div>
                <h1 class="header__title">SENSATEX</h1>
            </div>
            <p class="header__subtitle">Off-Grid Navigation System</p>
        </header>

        <!-- ── Compass Section ─────────────────────────────────────── -->
        <section class="compass-section glass" id="compass-section" aria-label="Compass direction indicator">
            <div class="compass-wrapper" id="compass-wrapper">
                <svg class="compass-svg" id="compass-svg" viewBox="0 0 240 240" xmlns="http://www.w3.org/2000/svg">
                    <!-- Rendered dynamically by app.js -->
                </svg>
            </div>

            <p class="compass-no-target" id="compass-no-target">
                Tap <strong>Navigate</strong> on a location to point the compass
            </p>

            <div class="compass-info" id="compass-info-grid" style="display:none;">
                <div class="compass-info__item">
                    <div class="compass-info__label">Heading</div>
                    <div class="compass-info__value" id="info-heading">—</div>
                </div>
                <div class="compass-info__item">
                    <div class="compass-info__label">Bearing</div>
                    <div class="compass-info__value compass-info__value--orange" id="info-bearing">—</div>
                </div>
                <div class="compass-info__item">
                    <div class="compass-info__label">Distance</div>
                    <div class="compass-info__value compass-info__value--green" id="info-distance">—</div>
                </div>
                <div class="compass-info__item">
                    <div class="compass-info__label">Target</div>
                    <div class="compass-info__value" id="info-target" style="font-family:'Inter',sans-serif;font-size:0.85rem;">—</div>
                </div>
            </div>
        </section>

        <!-- ── Locations Header ────────────────────────────────────── -->
        <div class="locations-header">
            <div class="locations-header__title">
                📍 Saved Locations
                <span class="locations-header__count" id="locations-count">0</span>
            </div>
            <button class="btn-add" onclick="openModal()" id="btn-add-location">
                + Add
            </button>
        </div>

        <!-- ── Location Cards ──────────────────────────────────────── -->
        <div class="locations-list" id="locations-list">
            <!-- Rendered dynamically -->
        </div>

        <!-- ── Status Bar ──────────────────────────────────────────── -->
        <footer class="status-bar glass" id="status-bar">
            <span id="status-gps"><span class="status-dot status-dot--yellow"></span>GPS Searching</span>
            <span id="status-compass"><span class="status-dot status-dot--yellow"></span>Compass</span>
            <span id="status-esp"><span class="status-dot status-dot--red"></span>ESP32</span>
        </footer>
    </div>

    <!-- ── Add Location Modal ──────────────────────────────────── -->
    <div class="modal-overlay" id="modal-overlay" onclick="if(event.target===this)closeModal()">
        <div class="modal" role="dialog" aria-label="Add new location">
            <div class="modal__header">
                <h2 class="modal__title">📍 Add Location</h2>
                <button class="btn-close" onclick="closeModal()" id="btn-close-modal" aria-label="Close">✕</button>
            </div>

            <!-- Location Name -->
            <div class="form-group">
                <label class="form-label" for="location-name">Location Name</label>
                <input class="form-input" type="text" id="location-name" placeholder="e.g. Safe House Alpha" autocomplete="off">
            </div>

            <!-- Coordinate System Tabs -->
            <div class="form-group">
                <label class="form-label">Coordinate System</label>
                <div class="coord-tabs" id="coord-tabs">
                    <button class="coord-tab coord-tab--active" data-system="latlon" onclick="setCoordSystem('latlon')" id="tab-latlon">Lat / Lon</button>
                    <button class="coord-tab" data-system="dms" onclick="setCoordSystem('dms')" id="tab-dms">DMS</button>
                    <button class="coord-tab" data-system="utm" onclick="setCoordSystem('utm')" id="tab-utm">UTM</button>
                </div>
            </div>

            <!-- ── Lat/Lon Fields ── -->
            <div class="form-group coord-fields coord-fields--latlon" id="fields-latlon">
                <div>
                    <label class="form-label" for="input-lat">Latitude</label>
                    <input class="form-input form-input--small" type="number" id="input-lat"
                           placeholder="53.2895" step="any" min="-90" max="90">
                </div>
                <div>
                    <label class="form-label" for="input-lon">Longitude</label>
                    <input class="form-input form-input--small" type="number" id="input-lon"
                           placeholder="-6.3768" step="any" min="-180" max="180">
                </div>
            </div>

            <!-- ── DMS Fields ── -->
            <div class="form-group" id="fields-dms" style="display:none;">
                <div class="dms-row-label">Latitude</div>
                <div class="coord-fields coord-fields--dms" style="margin-bottom:10px;">
                    <div>
                        <input class="form-input form-input--small" type="number" id="dms-lat-d" placeholder="53" min="0" max="90">
                    </div>
                    <div>
                        <input class="form-input form-input--small" type="number" id="dms-lat-m" placeholder="17" min="0" max="59">
                    </div>
                    <div>
                        <input class="form-input form-input--small" type="number" id="dms-lat-s" placeholder="22.2" step="any" min="0" max="59.99">
                    </div>
                    <div>
                        <select class="form-select" id="dms-lat-dir">
                            <option value="N">N</option>
                            <option value="S">S</option>
                        </select>
                    </div>
                </div>
                <div class="dms-row-label">Longitude</div>
                <div class="coord-fields coord-fields--dms">
                    <div>
                        <input class="form-input form-input--small" type="number" id="dms-lon-d" placeholder="6" min="0" max="180">
                    </div>
                    <div>
                        <input class="form-input form-input--small" type="number" id="dms-lon-m" placeholder="22" min="0" max="59">
                    </div>
                    <div>
                        <input class="form-input form-input--small" type="number" id="dms-lon-s" placeholder="36.5" step="any" min="0" max="59.99">
                    </div>
                    <div>
                        <select class="form-select" id="dms-lon-dir">
                            <option value="E">E</option>
                            <option value="W">W</option>
                        </select>
                    </div>
                </div>
            </div>

            <!-- ── UTM Fields ── -->
            <div class="form-group coord-fields coord-fields--utm" id="fields-utm" style="display:none;">
                <div>
                    <label class="form-label" for="utm-zone">Zone</label>
                    <input class="form-input form-input--small" type="number" id="utm-zone"
                           placeholder="29" min="1" max="60">
                </div>
                <div>
                    <label class="form-label" for="utm-letter">Band</label>
                    <input class="form-input form-input--small" type="text" id="utm-letter"
                           placeholder="U" maxlength="1" style="text-transform:uppercase;">
                </div>
                <div style="grid-column:1/2;">
                    <label class="form-label" for="utm-easting">Easting (m)</label>
                    <input class="form-input form-input--small" type="number" id="utm-easting"
                           placeholder="715830" step="any">
                </div>
                <div style="grid-column:2/4;">
                    <label class="form-label" for="utm-northing">Northing (m)</label>
                    <input class="form-input form-input--small" type="number" id="utm-northing"
                           placeholder="5905780" step="any">
                </div>
            </div>

            <!-- Error message -->
            <div class="form-error" id="form-error"></div>

            <!-- Submit -->
            <button class="btn-submit" onclick="handleSubmit()" id="btn-submit-location">
                Save Location
            </button>
        </div>
    </div>

    <!-- ── Toast ───────────────────────────────────────────────── -->
    <div class="toast" id="toast"></div>

    <script src="app.js"></script>
</body>
</html>
)rawliteral";

const char css_content[] PROGMEM = R"rawliteral(/* ═══════════════════════════════════════════════════════════════════════════
   SENSATEX RESCUE — Dark Glassmorphism Theme
   ═══════════════════════════════════════════════════════════════════════════ */

@import url('https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700;800&family=JetBrains+Mono:wght@400;500&display=swap');

/* ── Design Tokens ──────────────────────────────────────────────────────── */
:root {
  --bg-primary:    #06060f;
  --bg-secondary:  #0d0d1a;
  --bg-card:       rgba(15, 15, 30, 0.65);
  --bg-card-hover: rgba(20, 20, 45, 0.80);
  --bg-modal:      rgba(8, 8, 18, 0.92);

  --accent-cyan:   #00d4ff;
  --accent-orange: #ff6b35;
  --accent-green:  #00e676;
  --accent-red:    #ff3d5a;
  --accent-purple: #a855f7;

  --text-primary:   #e8eaf0;
  --text-secondary: #8890a4;
  --text-muted:     #4a5068;

  --glass-border:   rgba(255,255,255,0.06);
  --glass-shadow:   0 8px 32px rgba(0,0,0,0.4);
  --glass-blur:     16px;

  --radius-sm:  8px;
  --radius-md: 14px;
  --radius-lg: 20px;
  --radius-xl: 28px;

  --transition-fast:   0.15s cubic-bezier(0.4, 0, 0.2, 1);
  --transition-normal: 0.3s cubic-bezier(0.4, 0, 0.2, 1);
  --transition-slow:   0.5s cubic-bezier(0.4, 0, 0.2, 1);
}

/* ── Reset & Base ───────────────────────────────────────────────────────── */
*,
*::before,
*::after {
  margin: 0;
  padding: 0;
  box-sizing: border-box;
}

html {
  font-size: 16px;
  scroll-behavior: smooth;
  -webkit-tap-highlight-color: transparent;
}

body {
  font-family: 'Inter', -apple-system, BlinkMacSystemFont, sans-serif;
  background: var(--bg-primary);
  color: var(--text-primary);
  min-height: 100vh;
  overflow-x: hidden;
  line-height: 1.6;
}

/* Animated gradient background */
body::before {
  content: '';
  position: fixed;
  inset: 0;
  background:
    radial-gradient(ellipse 600px 400px at 20% 10%, rgba(0,212,255,0.06) 0%, transparent 70%),
    radial-gradient(ellipse 500px 500px at 80% 80%, rgba(255,107,53,0.04) 0%, transparent 70%),
    radial-gradient(ellipse 800px 600px at 50% 50%, rgba(168,85,247,0.03) 0%, transparent 70%);
  z-index: 0;
  pointer-events: none;
}

/* ── App Container ──────────────────────────────────────────────────────── */
.app {
  position: relative;
  z-index: 1;
  max-width: 480px;
  margin: 0 auto;
  padding: 16px;
  min-height: 100vh;
  display: flex;
  flex-direction: column;
}

/* ── Header ─────────────────────────────────────────────────────────────── */
.header {
  text-align: center;
  padding: 20px 0 12px;
}

.header__logo {
  display: inline-flex;
  align-items: center;
  gap: 10px;
  margin-bottom: 4px;
}

.header__icon {
  width: 36px;
  height: 36px;
  background: linear-gradient(135deg, var(--accent-cyan), var(--accent-purple));
  border-radius: 10px;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 18px;
  box-shadow: 0 0 20px rgba(0,212,255,0.3);
  animation: icon-glow 3s ease-in-out infinite alternate;
}

@keyframes icon-glow {
  from { box-shadow: 0 0 20px rgba(0,212,255,0.2); }
  to   { box-shadow: 0 0 30px rgba(0,212,255,0.5); }
}

.header__title {
  font-size: 1.55rem;
  font-weight: 800;
  letter-spacing: -0.5px;
  background: linear-gradient(135deg, #fff 30%, var(--accent-cyan));
  -webkit-background-clip: text;
  -webkit-text-fill-color: transparent;
  background-clip: text;
}

.header__subtitle {
  font-size: 0.78rem;
  color: var(--text-secondary);
  font-weight: 400;
  letter-spacing: 1.5px;
  text-transform: uppercase;
}

/* ── Glass Card (reusable) ──────────────────────────────────────────────── */
.glass {
  background: var(--bg-card);
  backdrop-filter: blur(var(--glass-blur));
  -webkit-backdrop-filter: blur(var(--glass-blur));
  border: 1px solid var(--glass-border);
  border-radius: var(--radius-lg);
  box-shadow: var(--glass-shadow);
}

/* ── Compass Section ────────────────────────────────────────────────────── */
.compass-section {
  margin: 12px 0;
  padding: 20px 16px;
  position: relative;
  overflow: hidden;
}

.compass-section::before {
  content: '';
  position: absolute;
  top: -50%;
  left: -50%;
  width: 200%;
  height: 200%;
  background: radial-gradient(circle at 50% 60%, rgba(0,212,255,0.04) 0%, transparent 50%);
  pointer-events: none;
}

.compass-wrapper {
  position: relative;
  width: 240px;
  height: 240px;
  margin: 0 auto 16px;
}

.compass-svg {
  width: 100%;
  height: 100%;
  filter: drop-shadow(0 0 12px rgba(0,212,255,0.15));
}

/* Compass info panel */
.compass-info {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 10px;
}

.compass-info__item {
  background: rgba(255,255,255,0.03);
  border-radius: var(--radius-sm);
  padding: 10px 12px;
  border: 1px solid rgba(255,255,255,0.04);
}

.compass-info__label {
  font-size: 0.65rem;
  color: var(--text-muted);
  text-transform: uppercase;
  letter-spacing: 1.2px;
  font-weight: 600;
  margin-bottom: 2px;
}

.compass-info__value {
  font-family: 'JetBrains Mono', monospace;
  font-size: 1.05rem;
  font-weight: 500;
  color: var(--accent-cyan);
}

.compass-info__value--orange {
  color: var(--accent-orange);
}

.compass-info__value--green {
  color: var(--accent-green);
}

.compass-no-target {
  text-align: center;
  color: var(--text-secondary);
  font-size: 0.85rem;
  padding: 8px 0;
}

/* ── Location List ──────────────────────────────────────────────────────── */
.locations-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin: 16px 0 10px;
  padding: 0 4px;
}

.locations-header__title {
  font-size: 1rem;
  font-weight: 700;
  display: flex;
  align-items: center;
  gap: 8px;
}

.locations-header__count {
  background: rgba(0,212,255,0.12);
  color: var(--accent-cyan);
  font-size: 0.7rem;
  font-weight: 700;
  padding: 2px 8px;
  border-radius: 20px;
  font-family: 'JetBrains Mono', monospace;
}

.btn-add {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding: 8px 16px;
  background: linear-gradient(135deg, var(--accent-cyan), #0098b3);
  color: #fff;
  font-size: 0.8rem;
  font-weight: 700;
  border: none;
  border-radius: var(--radius-xl);
  cursor: pointer;
  transition: all var(--transition-fast);
  box-shadow: 0 4px 15px rgba(0,212,255,0.25);
  letter-spacing: 0.3px;
}

.btn-add:hover {
  transform: translateY(-1px);
  box-shadow: 0 6px 20px rgba(0,212,255,0.4);
}

.btn-add:active {
  transform: translateY(0);
}

/* Location cards list */
.locations-list {
  display: flex;
  flex-direction: column;
  gap: 10px;
  margin-bottom: 20px;
}

.locations-list--empty {
  text-align: center;
  padding: 40px 20px;
  color: var(--text-secondary);
  font-size: 0.85rem;
}

.locations-list--empty .empty-icon {
  font-size: 2.5rem;
  margin-bottom: 12px;
  opacity: 0.4;
}

/* Single location card */
.location-card {
  padding: 14px 16px;
  transition: all var(--transition-normal);
  animation: card-enter 0.4s cubic-bezier(0.16, 1, 0.3, 1) both;
  position: relative;
  overflow: hidden;
}

.location-card::after {
  content: '';
  position: absolute;
  top: 0;
  left: 0;
  width: 3px;
  height: 100%;
  background: var(--accent-cyan);
  opacity: 0;
  transition: opacity var(--transition-fast);
  border-radius: 0 2px 2px 0;
}

.location-card:hover {
  background: var(--bg-card-hover);
  border-color: rgba(0,212,255,0.12);
  transform: translateX(2px);
}

.location-card:hover::after {
  opacity: 1;
}

.location-card--active {
  border-color: rgba(0,212,255,0.25) !important;
  background: rgba(0,212,255,0.06) !important;
}

.location-card--active::after {
  opacity: 1;
  background: var(--accent-orange);
}

@keyframes card-enter {
  from {
    opacity: 0;
    transform: translateY(12px) scale(0.97);
  }
  to {
    opacity: 1;
    transform: translateY(0) scale(1);
  }
}

.location-card__top {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  margin-bottom: 6px;
}

.location-card__name {
  font-size: 0.95rem;
  font-weight: 700;
  color: var(--text-primary);
}

.location-card__coords {
  font-family: 'JetBrains Mono', monospace;
  font-size: 0.72rem;
  color: var(--text-secondary);
  margin-top: 2px;
}

.location-card__distance {
  font-family: 'JetBrains Mono', monospace;
  font-size: 0.75rem;
  color: var(--accent-green);
  margin-top: 4px;
}

.location-card__actions {
  display: flex;
  gap: 8px;
  margin-top: 10px;
}

.btn-navigate,
.btn-delete {
  flex: 1;
  padding: 8px 0;
  font-size: 0.75rem;
  font-weight: 700;
  border: none;
  border-radius: var(--radius-sm);
  cursor: pointer;
  transition: all var(--transition-fast);
  letter-spacing: 0.3px;
  text-transform: uppercase;
}

.btn-navigate {
  background: linear-gradient(135deg, var(--accent-orange), #e05520);
  color: #fff;
  box-shadow: 0 3px 12px rgba(255,107,53,0.2);
}

.btn-navigate:hover {
  box-shadow: 0 5px 18px rgba(255,107,53,0.35);
  transform: translateY(-1px);
}

.btn-navigate--active {
  background: linear-gradient(135deg, var(--accent-green), #00b862);
  box-shadow: 0 3px 12px rgba(0,230,118,0.2);
}

.btn-delete {
  background: rgba(255,61,90,0.1);
  color: var(--accent-red);
  border: 1px solid rgba(255,61,90,0.15);
}

.btn-delete:hover {
  background: rgba(255,61,90,0.2);
  border-color: rgba(255,61,90,0.3);
}

/* ── Add Location Modal ─────────────────────────────────────────────────── */
.modal-overlay {
  position: fixed;
  inset: 0;
  background: rgba(0,0,0,0.7);
  backdrop-filter: blur(8px);
  -webkit-backdrop-filter: blur(8px);
  z-index: 100;
  display: flex;
  align-items: flex-end;
  justify-content: center;
  opacity: 0;
  visibility: hidden;
  transition: all var(--transition-normal);
}

.modal-overlay--visible {
  opacity: 1;
  visibility: visible;
}

.modal {
  background: var(--bg-modal);
  border: 1px solid var(--glass-border);
  border-radius: var(--radius-xl) var(--radius-xl) 0 0;
  width: 100%;
  max-width: 480px;
  padding: 24px 20px 32px;
  transform: translateY(100%);
  transition: transform 0.4s cubic-bezier(0.16, 1, 0.3, 1);
  max-height: 90vh;
  overflow-y: auto;
}

.modal-overlay--visible .modal {
  transform: translateY(0);
}

.modal__header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 20px;
}

.modal__title {
  font-size: 1.15rem;
  font-weight: 800;
}

.btn-close {
  width: 32px;
  height: 32px;
  border-radius: 50%;
  border: 1px solid var(--glass-border);
  background: rgba(255,255,255,0.05);
  color: var(--text-secondary);
  font-size: 1.1rem;
  cursor: pointer;
  display: flex;
  align-items: center;
  justify-content: center;
  transition: all var(--transition-fast);
}

.btn-close:hover {
  background: rgba(255,61,90,0.15);
  color: var(--accent-red);
  border-color: rgba(255,61,90,0.3);
}

/* Form */
.form-group {
  margin-bottom: 16px;
}

.form-label {
  display: block;
  font-size: 0.72rem;
  font-weight: 600;
  color: var(--text-secondary);
  text-transform: uppercase;
  letter-spacing: 1.2px;
  margin-bottom: 6px;
}

.form-input {
  width: 100%;
  padding: 12px 14px;
  background: rgba(255,255,255,0.04);
  border: 1px solid rgba(255,255,255,0.08);
  border-radius: var(--radius-sm);
  color: var(--text-primary);
  font-family: 'Inter', sans-serif;
  font-size: 0.9rem;
  outline: none;
  transition: all var(--transition-fast);
}

.form-input:focus {
  border-color: var(--accent-cyan);
  background: rgba(0,212,255,0.04);
  box-shadow: 0 0 0 3px rgba(0,212,255,0.08);
}

.form-input::placeholder {
  color: var(--text-muted);
}

/* Coordinate system selector (pill tabs) */
.coord-tabs {
  display: flex;
  background: rgba(255,255,255,0.03);
  border-radius: var(--radius-md);
  padding: 3px;
  border: 1px solid rgba(255,255,255,0.05);
  gap: 3px;
}

.coord-tab {
  flex: 1;
  padding: 9px 0;
  text-align: center;
  font-size: 0.75rem;
  font-weight: 700;
  color: var(--text-secondary);
  background: transparent;
  border: none;
  border-radius: 11px;
  cursor: pointer;
  transition: all var(--transition-fast);
  letter-spacing: 0.5px;
  text-transform: uppercase;
}

.coord-tab:hover {
  color: var(--text-primary);
  background: rgba(255,255,255,0.04);
}

.coord-tab--active {
  background: linear-gradient(135deg, var(--accent-cyan), #0098b3) !important;
  color: #fff !important;
  box-shadow: 0 2px 10px rgba(0,212,255,0.25);
}

/* Dynamic coordinate input fields */
.coord-fields {
  display: grid;
  gap: 10px;
}

.coord-fields--latlon {
  grid-template-columns: 1fr 1fr;
}

.coord-fields--dms {
  grid-template-columns: 1fr 1fr 1fr auto;
}

.coord-fields--utm {
  grid-template-columns: 1fr 1fr 1fr;
}

.form-input--small {
  padding: 10px 10px;
  font-size: 0.82rem;
  font-family: 'JetBrains Mono', monospace;
}

.form-select {
  width: 100%;
  padding: 10px 10px;
  background: rgba(255,255,255,0.04);
  border: 1px solid rgba(255,255,255,0.08);
  border-radius: var(--radius-sm);
  color: var(--text-primary);
  font-family: 'Inter', sans-serif;
  font-size: 0.82rem;
  outline: none;
  cursor: pointer;
  transition: all var(--transition-fast);
  -webkit-appearance: none;
  appearance: none;
}

.form-select:focus {
  border-color: var(--accent-cyan);
}

.form-select option {
  background: #1a1a2e;
  color: var(--text-primary);
}

.dms-row-label {
  font-size: 0.68rem;
  color: var(--text-muted);
  text-transform: uppercase;
  letter-spacing: 1px;
  font-weight: 600;
  margin-bottom: 4px;
  margin-top: 6px;
}

.btn-submit {
  width: 100%;
  padding: 14px;
  background: linear-gradient(135deg, var(--accent-cyan), var(--accent-purple));
  color: #fff;
  font-size: 0.9rem;
  font-weight: 800;
  border: none;
  border-radius: var(--radius-md);
  cursor: pointer;
  transition: all var(--transition-fast);
  box-shadow: 0 4px 20px rgba(0,212,255,0.25);
  letter-spacing: 0.5px;
  text-transform: uppercase;
  margin-top: 8px;
}

.btn-submit:hover {
  transform: translateY(-1px);
  box-shadow: 0 6px 25px rgba(0,212,255,0.4);
}

.btn-submit:active {
  transform: translateY(0);
}

.form-error {
  color: var(--accent-red);
  font-size: 0.75rem;
  margin-top: 6px;
  display: none;
}

.form-error--visible {
  display: block;
  animation: shake 0.4s ease;
}

@keyframes shake {
  0%, 100% { transform: translateX(0); }
  20%, 60% { transform: translateX(-4px); }
  40%, 80% { transform: translateX(4px); }
}

/* ── Status Bar ─────────────────────────────────────────────────────────── */
.status-bar {
  margin-top: auto;
  padding: 12px 16px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  font-size: 0.7rem;
  color: var(--text-muted);
  margin-bottom: 8px;
}

.status-dot {
  display: inline-block;
  width: 7px;
  height: 7px;
  border-radius: 50%;
  margin-right: 5px;
  vertical-align: middle;
}

.status-dot--green {
  background: var(--accent-green);
  box-shadow: 0 0 6px rgba(0,230,118,0.5);
}

.status-dot--red {
  background: var(--accent-red);
  box-shadow: 0 0 6px rgba(255,61,90,0.5);
}

.status-dot--yellow {
  background: #ffc107;
  box-shadow: 0 0 6px rgba(255,193,7,0.5);
  animation: pulse-dot 2s ease-in-out infinite;
}

@keyframes pulse-dot {
  0%, 100% { opacity: 1; }
  50%      { opacity: 0.4; }
}

/* ── Toast notifications ────────────────────────────────────────────────── */
.toast {
  position: fixed;
  top: 20px;
  left: 50%;
  transform: translateX(-50%) translateY(-100px);
  background: var(--bg-card);
  backdrop-filter: blur(20px);
  -webkit-backdrop-filter: blur(20px);
  border: 1px solid var(--glass-border);
  border-radius: var(--radius-md);
  padding: 12px 20px;
  font-size: 0.85rem;
  font-weight: 600;
  color: var(--text-primary);
  box-shadow: 0 10px 40px rgba(0,0,0,0.5);
  z-index: 200;
  transition: transform 0.4s cubic-bezier(0.16, 1, 0.3, 1);
  pointer-events: none;
  max-width: 360px;
  text-align: center;
}

.toast--visible {
  transform: translateX(-50%) translateY(0);
}

.toast--success {
  border-color: rgba(0,230,118,0.3);
}

.toast--error {
  border-color: rgba(255,61,90,0.3);
}

/* ── Utility ────────────────────────────────────────────────────────────── */
.mono {
  font-family: 'JetBrains Mono', monospace;
}

.sr-only {
  position: absolute;
  width: 1px;
  height: 1px;
  overflow: hidden;
  clip: rect(0,0,0,0);
  white-space: nowrap;
  border: 0;
}

/* ── Responsive ─────────────────────────────────────────────────────────── */
@media (min-width: 500px) {
  .app {
    padding: 24px;
  }

  .compass-wrapper {
    width: 280px;
    height: 280px;
  }

  .modal {
    border-radius: var(--radius-xl);
    margin-bottom: 40px;
  }

  .modal-overlay {
    align-items: center;
  }
}
)rawliteral";

const char js_content[] PROGMEM = R"rawliteral(/* ═══════════════════════════════════════════════════════════════════════════
   SENSATEX RESCUE — Application Logic
   ═══════════════════════════════════════════════════════════════════════════ */

// ── State ──────────────────────────────────────────────────────────────────
const STORAGE_KEY = 'sensatex_locations';

let locations = [];          // Array of { id, name, lat, lon, createdAt }
let activeLocationId = null; // Currently navigating to
let deviceHeading = null;    // From DeviceOrientationEvent (degrees)
let userLat = null;          // User's current GPS position
let userLon = null;
let geoWatchId = null;
let compassAnimFrame = null;
let smoothedHeading = null;  // Smoothed heading for animation
let esp32Connected = false;

// ═══════════════════════════════════════════════════════════════════════════
//  COORDINATE CONVERSION
// ═══════════════════════════════════════════════════════════════════════════

/**
 * DMS (Degrees, Minutes, Seconds) → Decimal Degrees
 * @param {number} deg - Degrees
 * @param {number} min - Minutes
 * @param {number} sec - Seconds
 * @param {string} dir - N, S, E, or W
 * @returns {number} Decimal degrees (negative for S/W)
 */
function dmsToDecimal(deg, min, sec, dir) {
  let dd = Math.abs(deg) + min / 60 + sec / 3600;
  if (dir === 'S' || dir === 'W') dd *= -1;
  return dd;
}

/**
 * Decimal degrees → DMS string
 */
function decimalToDms(dd, isLat) {
  const dir = isLat ? (dd >= 0 ? 'N' : 'S') : (dd >= 0 ? 'E' : 'W');
  dd = Math.abs(dd);
  const d = Math.floor(dd);
  const mFloat = (dd - d) * 60;
  const m = Math.floor(mFloat);
  const s = ((mFloat - m) * 60).toFixed(2);
  return `${d}°${m}'${s}"${dir}`;
}

/**
 * UTM → Lat/Lon (WGS84)
 * Implements inverse transverse Mercator projection.
 * Based on Karney (2011) simplified for standard use.
 *
 * @param {number} zone    - UTM zone number (1-60)
 * @param {string} letter  - UTM latitude band letter
 * @param {number} easting - Easting in metres
 * @param {number} northing - Northing in metres
 * @returns {{ lat: number, lon: number }}
 */
function utmToLatLon(zone, letter, easting, northing) {
  // WGS84 ellipsoid constants
  const a = 6378137.0;                        // semi-major axis
  const f = 1 / 298.257223563;                // flattening
  const e = Math.sqrt(2 * f - f * f);         // first eccentricity
  const e2 = e * e;
  const ePrime2 = e2 / (1 - e2);              // second eccentricity squared
  const k0 = 0.9996;                          // scale factor at central meridian
  const n = f / (2 - f);
  const n2 = n * n;
  const n3 = n * n2;
  const n4 = n * n3;

  const isSouthern = letter.toUpperCase() < 'N';

  // Remove false easting/northing
  const x = easting - 500000;
  const y = isSouthern ? northing - 10000000 : northing;

  // Meridional arc constants (A series)
  const A0 = a / (1 + n) * (1 + n2 / 4 + n4 / 64);

  const xi = y / (k0 * A0);
  const eta = x / (k0 * A0);

  // Fourier coefficients for inverse (β series)
  const b1 = 0.5 * n - (2 / 3) * n2 + (37 / 96) * n3 - (1 / 360) * n4;
  const b2 = (1 / 48) * n2 + (1 / 15) * n3 - (437 / 1440) * n4;
  const b3 = (17 / 480) * n3 - (37 / 840) * n4;
  const b4 = (4397 / 161280) * n4;

  const xiPrime = xi
    - b1 * Math.sin(2 * xi) * Math.cosh(2 * eta)
    - b2 * Math.sin(4 * xi) * Math.cosh(4 * eta)
    - b3 * Math.sin(6 * xi) * Math.cosh(6 * eta)
    - b4 * Math.sin(8 * xi) * Math.cosh(8 * eta);

  const etaPrime = eta
    - b1 * Math.cos(2 * xi) * Math.sinh(2 * eta)
    - b2 * Math.cos(4 * xi) * Math.sinh(4 * eta)
    - b3 * Math.cos(6 * xi) * Math.sinh(6 * eta)
    - b4 * Math.cos(8 * xi) * Math.sinh(8 * eta);

  const chi = Math.asin(Math.sin(xiPrime) / Math.cosh(etaPrime));

  // Convert conformal latitude to geodetic latitude
  const d1 = 2 * n - (2 / 3) * n2 - 2 * n3 + (116 / 45) * n4;
  const d2 = (7 / 3) * n2 - (8 / 5) * n3 - (227 / 45) * n4;
  const d3 = (56 / 15) * n3 - (136 / 35) * n4;
  const d4 = (4279 / 630) * n4;

  const lat = chi
    + d1 * Math.sin(2 * chi)
    + d2 * Math.sin(4 * chi)
    + d3 * Math.sin(6 * chi)
    + d4 * Math.sin(8 * chi);

  const lon0 = ((zone - 1) * 6 - 180 + 3) * Math.PI / 180; // central meridian
  const lon = lon0 + Math.atan2(Math.sinh(etaPrime), Math.cos(xiPrime));

  return {
    lat: lat * 180 / Math.PI,
    lon: lon * 180 / Math.PI
  };
}

// ═══════════════════════════════════════════════════════════════════════════
//  HAVERSINE — Distance & Bearing
// ═══════════════════════════════════════════════════════════════════════════

function toRad(deg) { return deg * Math.PI / 180; }
function toDeg(rad) { return rad * 180 / Math.PI; }

/**
 * Haversine distance in metres
 */
function haversineDistance(lat1, lon1, lat2, lon2) {
  const R = 6371000;
  const dLat = toRad(lat2 - lat1);
  const dLon = toRad(lon2 - lon1);
  const a = Math.sin(dLat / 2) ** 2
          + Math.cos(toRad(lat1)) * Math.cos(toRad(lat2)) * Math.sin(dLon / 2) ** 2;
  return R * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}

/**
 * Initial bearing from (lat1,lon1) to (lat2,lon2) in degrees (0-360)
 */
function bearing(lat1, lon1, lat2, lon2) {
  const dLon = toRad(lon2 - lon1);
  const y = Math.sin(dLon) * Math.cos(toRad(lat2));
  const x = Math.cos(toRad(lat1)) * Math.sin(toRad(lat2))
          - Math.sin(toRad(lat1)) * Math.cos(toRad(lat2)) * Math.cos(dLon);
  return (toDeg(Math.atan2(y, x)) + 360) % 360;
}

/**
 * Compass cardinal direction from degrees
 */
function cardinalDir(deg) {
  const dirs = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];
  return dirs[Math.round(deg / 45) % 8];
}

/**
 * Format distance human-readable
 */
function formatDistance(meters) {
  if (meters >= 1000) return (meters / 1000).toFixed(2) + ' km';
  return meters.toFixed(0) + ' m';
}

// ═══════════════════════════════════════════════════════════════════════════
//  LOCAL STORAGE
// ═══════════════════════════════════════════════════════════════════════════

function loadLocations() {
  try {
    const data = localStorage.getItem(STORAGE_KEY);
    locations = data ? JSON.parse(data) : getDefaultLocations
    ();
    if (!data) saveLocations(); // persist defaults on first load
  } catch {
    locations = getDefaultLocations();
    saveLocations();
  }
}

function saveLocations() {
  localStorage.setItem(STORAGE_KEY, JSON.stringify(locations));
}

function getDefaultLocations() {
  return [
    { id: genId(), name: 'Tallaght Hospital',  lat: 53.2895, lon: -6.3768, createdAt: Date.now() },
    { id: genId(), name: 'Garda Station',      lat: 53.2875, lon: -6.3712, createdAt: Date.now() },
    { id: genId(), name: 'Embassy',            lat: 53.3283, lon: -6.2305, createdAt: Date.now() },
    { id: genId(), name: 'Kebab',              lat: 53.288259, lon: -6.36338, createdAt: Date.now()},
  ];
}

function genId() {
  return Date.now().toString(36) + Math.random().toString(36).slice(2, 7);
}

function addLocation(name, lat, lon) {
  const loc = { id: genId(), name, lat, lon, createdAt: Date.now() };
  locations.push(loc);
  saveLocations();
  renderLocations();
  showToast(`📍 ${name} added`, 'success');
  return loc;
}

function deleteLocation(id) {
  const loc = locations.find(l => l.id === id);
  locations = locations.filter(l => l.id !== id);
  if (activeLocationId === id) activeLocationId = null;
  saveLocations();
  renderLocations();
  updateCompassInfo();
  if (loc) showToast(`🗑️ ${loc.name} removed`, 'success');
}

function navigateToLocation(id) {
  if (activeLocationId === id) {
    activeLocationId = null;   // toggle off
  } else {
    activeLocationId = id;
  }
  renderLocations();
  updateCompassInfo();

  // Send to ESP32 if connected
  if (activeLocationId) {
    const loc = locations.find(l => l.id === id);
    if (loc) sendToESP32(loc);
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  ESP32 COMMUNICATION
// ═══════════════════════════════════════════════════════════════════════════

async function sendToESP32(loc) {
  try {
    const url = `/set?lat=${loc.lat}&lon=${loc.lon}&name=${encodeURIComponent(loc.name)}`;
    const resp = await fetch(url, { signal: AbortSignal.timeout(3000) });
    if (resp.ok) {
      esp32Connected = true;
      updateStatusBar();
      showToast(`🛰️ Sent to radar: ${loc.name}`, 'success');
    }
  } catch {
    // Not on ESP32 network — that's fine
    esp32Connected = false;
    updateStatusBar();
  }
}

async function pollESP32() {
  try {
    // Add timestamp to prevent caching
    const resp = await fetch('/gps?t=' + Date.now(), { signal: AbortSignal.timeout(2000) });
    const data = await resp.json();
    esp32Connected = true;
    
    if (data.valid) {
      // OVERRIDE phone's GPS with ESP32's high-precision GNSS
      userLat = data.lat;
      userLon = data.lon;
      
      // If the phone blocks DeviceOrientation (HTTP), use ESP32's GPS Course Over Ground (if moving > 0.8 km/h)
      if (deviceHeading === null && data.heading >= 0 && data.speed > 0.8) {
          deviceHeading = data.heading;
      }
      
      updateDistances();
      updateCompassInfo();
    }
  } catch {
    esp32Connected = false;
  }
  updateStatusBar();
}

// ═══════════════════════════════════════════════════════════════════════════
//  COMPASS & DEVICE ORIENTATION
// ═══════════════════════════════════════════════════════════════════════════

function initCompass() {
  // Request orientation permission (required on iOS 13+)
  if (typeof DeviceOrientationEvent !== 'undefined' &&
      typeof DeviceOrientationEvent.requestPermission === 'function') {
    // iOS 13+ requires user gesture
    document.getElementById('compass-wrapper').addEventListener('click', async () => {
      try {
        const perm = await DeviceOrientationEvent.requestPermission();
        if (perm === 'granted') startOrientationListener();
      } catch (err) {
        console.warn('Orientation permission denied:', err);
      }
    }, { once: true });
  } else {
    startOrientationListener();
  }
}

function startOrientationListener() {
  window.addEventListener('deviceorientationabsolute', handleOrientation, true);
  window.addEventListener('deviceorientation', handleOrientation, true);
}

function handleOrientation(event) {
  let heading = null;

  if (event.webkitCompassHeading !== undefined) {
    // iOS Safari — compass heading is already 0=N clockwise
    heading = event.webkitCompassHeading;
  } else if (event.alpha !== null) {
    // Android / Chrome — alpha is degrees from North
    if (event.absolute) {
      heading = (360 - event.alpha) % 360;
    } else {
      heading = (360 - event.alpha) % 360;
    }
  }

  if (heading !== null) {
    deviceHeading = heading;
  }
}

function initGeolocation() {
  if ('geolocation' in navigator) {
    geoWatchId = navigator.geolocation.watchPosition(
      (pos) => {
        userLat = pos.coords.latitude;
        userLon = pos.coords.longitude;
        updateDistances();
        updateStatusBar();
      },
      (err) => {
        console.warn('Geolocation error:', err.message);
        updateStatusBar();
      },
      { enableHighAccuracy: true, maximumAge: 5000, timeout: 15000 }
    );
  }
}

// ── Compass rendering loop ─────────────────────────────────────────────────
function startCompassLoop() {
  function frame() {
    drawCompass();
    updateCompassInfo();
    compassAnimFrame = requestAnimationFrame(frame);
  }
  compassAnimFrame = requestAnimationFrame(frame);
}

function drawCompass() {
  const svg = document.getElementById('compass-svg');
  if (!svg) return;

  const cx = 120, cy = 120, r = 100;
  const heading = deviceHeading !== null ? deviceHeading : 0;

  // Smooth heading for nice animation
  if (smoothedHeading === null) {
    smoothedHeading = heading;
  } else {
    let diff = heading - smoothedHeading;
    // Handle wrapping around 360
    if (diff > 180) diff -= 360;
    if (diff < -180) diff += 360;
    smoothedHeading = (smoothedHeading + diff * 0.12 + 360) % 360;
  }

  // Calculate target bearing relative to heading
  let targetBearing = null;
  let targetDist = null;
  if (activeLocationId && userLat !== null) {
    const loc = locations.find(l => l.id === activeLocationId);
    if (loc) {
      const absBearing = bearing(userLat, userLon, loc.lat, loc.lon);
      targetBearing = (absBearing - smoothedHeading + 360) % 360;
      targetDist = haversineDistance(userLat, userLon, loc.lat, loc.lon);
    }
  }

  // Build SVG content
  let html = '';

  // ── Outer glow ring ──
  html += `<circle cx="${cx}" cy="${cy}" r="${r + 8}" fill="none" stroke="rgba(0,212,255,0.06)" stroke-width="1"/>`;

  // ── Tick marks ── (rotate by -heading so they stay fixed to North)
  const rot = -smoothedHeading;
  html += `<g transform="rotate(${rot}, ${cx}, ${cy})">`;

  for (let i = 0; i < 360; i += 5) {
    const isMajor = i % 30 === 0;
    const isCardinal = i % 90 === 0;
    const len = isCardinal ? 16 : isMajor ? 10 : 5;
    const sw = isCardinal ? 2.5 : isMajor ? 1.5 : 0.8;
    const color = isCardinal ? '#00d4ff' : isMajor ? 'rgba(255,255,255,0.35)' : 'rgba(255,255,255,0.12)';
    const rad = (i * Math.PI) / 180;
    const x1 = cx + (r - len) * Math.sin(rad);
    const y1 = cy - (r - len) * Math.cos(rad);
    const x2 = cx + r * Math.sin(rad);
    const y2 = cy - r * Math.cos(rad);
    html += `<line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}" stroke="${color}" stroke-width="${sw}" stroke-linecap="round"/>`;
  }

  // ── Cardinal letters ──
  const cardinals = [
    { label: 'N', deg: 0,   color: '#ff6b35' },
    { label: 'E', deg: 90,  color: '#e8eaf0' },
    { label: 'S', deg: 180, color: '#e8eaf0' },
    { label: 'W', deg: 270, color: '#e8eaf0' },
  ];
  for (const c of cardinals) {
    const rad = (c.deg * Math.PI) / 180;
    const tx = cx + (r - 26) * Math.sin(rad);
    const ty = cy - (r - 26) * Math.cos(rad);
    html += `<text x="${tx}" y="${ty}" text-anchor="middle" dominant-baseline="central"
              font-family="Inter,sans-serif" font-size="14" font-weight="800"
              fill="${c.color}">${c.label}</text>`;
  }

  // ── Degree numbers (every 30°) ──
  for (let i = 30; i < 360; i += 30) {
    if (i % 90 === 0) continue; // skip cardinals
    const rad = (i * Math.PI) / 180;
    const tx = cx + (r - 26) * Math.sin(rad);
    const ty = cy - (r - 26) * Math.cos(rad);
    html += `<text x="${tx}" y="${ty}" text-anchor="middle" dominant-baseline="central"
              font-family="JetBrains Mono,monospace" font-size="9" font-weight="500"
              fill="rgba(255,255,255,0.3)">${i}</text>`;
  }

  html += '</g>'; // end rotation group

  // ── Fixed heading indicator (top triangle) ──
  html += `<polygon points="${cx},${cy - r - 4} ${cx - 6},${cy - r + 8} ${cx + 6},${cy - r + 8}"
            fill="#ff6b35" stroke="none"/>`;

  // ── Centre dot ──
  html += `<circle cx="${cx}" cy="${cy}" r="4" fill="rgba(0,212,255,0.5)"/>`;
  html += `<circle cx="${cx}" cy="${cy}" r="2" fill="#00d4ff"/>`;

  // ── Target direction arrow ──
  if (targetBearing !== null) {
    const arrowLen = r - 36;
    const arrowRad = (targetBearing * Math.PI) / 180;
    const tipX = cx + arrowLen * Math.sin(arrowRad);
    const tipY = cy - arrowLen * Math.cos(arrowRad);

    // Arrowhead
    const headLen = 14;
    const headW = 8;
    const perpRad = arrowRad + Math.PI / 2;
    const baseX = tipX - headLen * Math.sin(arrowRad);
    const baseY = tipY + headLen * Math.cos(arrowRad);
    const lx = baseX + headW * Math.sin(perpRad);
    const ly = baseY - headW * Math.cos(perpRad);
    const rx = baseX - headW * Math.sin(perpRad);
    const ry = baseY + headW * Math.cos(perpRad);

    // Arrow line from centre
    html += `<line x1="${cx}" y1="${cy}" x2="${baseX}" y2="${baseY}"
              stroke="#ff6b35" stroke-width="3" stroke-linecap="round" opacity="0.8"/>`;

    // Arrow head
    html += `<polygon points="${tipX},${tipY} ${lx},${ly} ${rx},${ry}"
              fill="#ff6b35" stroke="none">
              <animate attributeName="opacity" values="0.7;1;0.7" dur="2s" repeatCount="indefinite"/>
             </polygon>`;

    // Glow
    html += `<circle cx="${tipX}" cy="${tipY}" r="6" fill="none" stroke="rgba(255,107,53,0.3)" stroke-width="2">
              <animate attributeName="r" values="6;10;6" dur="2s" repeatCount="indefinite"/>
              <animate attributeName="opacity" values="0.5;0;0.5" dur="2s" repeatCount="indefinite"/>
             </circle>`;
  }

  svg.innerHTML = html;
}

function updateCompassInfo() {
  const headingEl = document.getElementById('info-heading');
  const bearingEl = document.getElementById('info-bearing');
  const distanceEl = document.getElementById('info-distance');
  const targetEl = document.getElementById('info-target');
  const noTargetEl = document.getElementById('compass-no-target');
  const infoGrid = document.getElementById('compass-info-grid');

  if (!headingEl) return;

  // Heading
  if (deviceHeading !== null) {
    headingEl.textContent = `${Math.round(deviceHeading)}° ${cardinalDir(deviceHeading)}`;
  } else {
    headingEl.textContent = '—';
  }

  // Active target info
  if (activeLocationId) {
    const loc = locations.find(l => l.id === activeLocationId);
    if (loc) {
      if (noTargetEl) noTargetEl.style.display = 'none';
      if (infoGrid) infoGrid.style.display = 'grid';

      targetEl.textContent = loc.name;

      if (userLat !== null) {
        const brg = bearing(userLat, userLon, loc.lat, loc.lon);
        const dist = haversineDistance(userLat, userLon, loc.lat, loc.lon);
        bearingEl.textContent = `${Math.round(brg)}° ${cardinalDir(brg)}`;
        distanceEl.textContent = formatDistance(dist);
      } else {
        bearingEl.textContent = '—';
        distanceEl.textContent = '—';
      }
    }
  } else {
    if (noTargetEl) noTargetEl.style.display = 'block';
    if (infoGrid) infoGrid.style.display = 'none';
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  UI RENDERING
// ═══════════════════════════════════════════════════════════════════════════

function renderLocations() {
  const list = document.getElementById('locations-list');
  const countEl = document.getElementById('locations-count');
  if (!list) return;

  countEl.textContent = locations.length;

  if (locations.length === 0) {
    list.innerHTML = `
      <div class="locations-list--empty glass">
        <div class="empty-icon">📍</div>
        <p>No locations saved yet</p>
        <p style="margin-top:4px;font-size:0.75rem;">Tap <strong>+ Add</strong> to save a coordinate</p>
      </div>`;
    return;
  }

  let html = '';
  locations.forEach((loc, i) => {
    const isActive = loc.id === activeLocationId;
    let distText = '';
    if (userLat !== null) {
      const d = haversineDistance(userLat, userLon, loc.lat, loc.lon);
      distText = formatDistance(d);
    }

    const latStr = loc.lat >= 0 ? `${loc.lat.toFixed(6)}°N` : `${Math.abs(loc.lat).toFixed(6)}°S`;
    const lonStr = loc.lon >= 0 ? `${loc.lon.toFixed(6)}°E` : `${Math.abs(loc.lon).toFixed(6)}°W`;

    html += `
      <div class="location-card glass ${isActive ? 'location-card--active' : ''}"
           style="animation-delay: ${i * 0.06}s" id="loc-card-${loc.id}">
        <div class="location-card__top">
          <div>
            <div class="location-card__name">${escHtml(loc.name)}</div>
            <div class="location-card__coords">${latStr}, ${lonStr}</div>
            ${distText ? `<div class="location-card__distance">📏 ${distText}</div>` : ''}
          </div>
        </div>
        <div class="location-card__actions">
          <button class="btn-navigate ${isActive ? 'btn-navigate--active' : ''}"
                  onclick="navigateToLocation('${loc.id}')" id="btn-nav-${loc.id}">
            ${isActive ? '✓ Navigating' : '🧭 Navigate'}
          </button>
          <button class="btn-delete" onclick="deleteLocation('${loc.id}')" id="btn-del-${loc.id}">
            ✕ Delete
          </button>
        </div>
      </div>`;
  });

  list.innerHTML = html;
}

function updateDistances() {
  // Update distance text on all visible cards without full re-render
  locations.forEach(loc => {
    const card = document.getElementById(`loc-card-${loc.id}`);
    if (!card || userLat === null) return;
    const distEl = card.querySelector('.location-card__distance');
    const d = haversineDistance(userLat, userLon, loc.lat, loc.lon);
    const text = `📏 ${formatDistance(d)}`;
    if (distEl) {
      distEl.textContent = text;
    } else {
      // Insert distance element
      const coordsEl = card.querySelector('.location-card__coords');
      if (coordsEl) {
        const el = document.createElement('div');
        el.className = 'location-card__distance';
        el.textContent = text;
        coordsEl.insertAdjacentElement('afterend', el);
      }
    }
  });
}

function updateStatusBar() {
  const gpsEl = document.getElementById('status-gps');
  const espEl = document.getElementById('status-esp');
  const compassEl = document.getElementById('status-compass');

  if (gpsEl) {
    if (userLat !== null) {
      gpsEl.innerHTML = `<span class="status-dot status-dot--green"></span>GPS Fix`;
    } else {
      gpsEl.innerHTML = `<span class="status-dot status-dot--yellow"></span>GPS Searching`;
    }
  }
  if (espEl) {
    if (esp32Connected) {
      espEl.innerHTML = `<span class="status-dot status-dot--green"></span>ESP32`;
    } else {
      espEl.innerHTML = `<span class="status-dot status-dot--red"></span>ESP32`;
    }
  }
  if (compassEl) {
    if (deviceHeading !== null) {
      compassEl.innerHTML = `<span class="status-dot status-dot--green"></span>Compass`;
    } else {
      compassEl.innerHTML = `<span class="status-dot status-dot--yellow"></span>Compass`;
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  MODAL — Add Location
// ═══════════════════════════════════════════════════════════════════════════

let currentCoordSystem = 'latlon';

function openModal() {
  document.getElementById('modal-overlay').classList.add('modal-overlay--visible');
  document.getElementById('location-name').focus();
  setCoordSystem('latlon');
}

function closeModal() {
  document.getElementById('modal-overlay').classList.remove('modal-overlay--visible');
  clearForm();
}

function setCoordSystem(system) {
  currentCoordSystem = system;

  // Update tab styling
  document.querySelectorAll('.coord-tab').forEach(tab => {
    tab.classList.toggle('coord-tab--active', tab.dataset.system === system);
  });

  // Show correct fields
  document.getElementById('fields-latlon').style.display = system === 'latlon' ? 'grid' : 'none';
  document.getElementById('fields-dms').style.display    = system === 'dms'    ? 'block' : 'none';
  document.getElementById('fields-utm').style.display    = system === 'utm'    ? 'grid'  : 'none';

  hideError();
}

function handleSubmit() {
  const name = document.getElementById('location-name').value.trim();
  if (!name) {
    showError('Please enter a location name');
    return;
  }

  let lat, lon;

  try {
    if (currentCoordSystem === 'latlon') {
      lat = parseFloat(document.getElementById('input-lat').value);
      lon = parseFloat(document.getElementById('input-lon').value);
      if (isNaN(lat) || isNaN(lon)) throw new Error('Invalid coordinates');
      if (lat < -90 || lat > 90) throw new Error('Latitude must be between -90 and 90');
      if (lon < -180 || lon > 180) throw new Error('Longitude must be between -180 and 180');

    } else if (currentCoordSystem === 'dms') {
      const latD = parseInt(document.getElementById('dms-lat-d').value) || 0;
      const latM = parseInt(document.getElementById('dms-lat-m').value) || 0;
      const latS = parseFloat(document.getElementById('dms-lat-s').value) || 0;
      const latDir = document.getElementById('dms-lat-dir').value;

      const lonD = parseInt(document.getElementById('dms-lon-d').value) || 0;
      const lonM = parseInt(document.getElementById('dms-lon-m').value) || 0;
      const lonS = parseFloat(document.getElementById('dms-lon-s').value) || 0;
      const lonDir = document.getElementById('dms-lon-dir').value;

      if (latD === 0 && latM === 0 && latS === 0 && lonD === 0 && lonM === 0 && lonS === 0) {
        throw new Error('Please enter DMS coordinates');
      }

      lat = dmsToDecimal(latD, latM, latS, latDir);
      lon = dmsToDecimal(lonD, lonM, lonS, lonDir);

    } else if (currentCoordSystem === 'utm') {
      const zone = parseInt(document.getElementById('utm-zone').value);
      const letter = document.getElementById('utm-letter').value.trim().toUpperCase();
      const easting = parseFloat(document.getElementById('utm-easting').value);
      const northing = parseFloat(document.getElementById('utm-northing').value);

      if (isNaN(zone) || zone < 1 || zone > 60) throw new Error('UTM zone must be 1-60');
      if (!/^[C-X]$/i.test(letter) || letter === 'I' || letter === 'O') throw new Error('Invalid UTM band letter');
      if (isNaN(easting) || isNaN(northing)) throw new Error('Invalid UTM easting/northing');

      const result = utmToLatLon(zone, letter, easting, northing);
      lat = result.lat;
      lon = result.lon;
    }

    // Validate result
    if (isNaN(lat) || isNaN(lon) || !isFinite(lat) || !isFinite(lon)) {
      throw new Error('Conversion resulted in invalid coordinates');
    }

    addLocation(name, lat, lon);
    closeModal();

  } catch (err) {
    showError(err.message);
  }
}

function showError(msg) {
  const el = document.getElementById('form-error');
  el.textContent = msg;
  el.classList.add('form-error--visible');
}

function hideError() {
  const el = document.getElementById('form-error');
  if (el) {
    el.classList.remove('form-error--visible');
    el.textContent = '';
  }
}

function clearForm() {
  document.getElementById('location-name').value = '';
  document.querySelectorAll('.modal input[type="number"], .modal input[type="text"]')
    .forEach(el => { if (el.id !== 'location-name') el.value = ''; });
  hideError();
}

// ═══════════════════════════════════════════════════════════════════════════
//  TOAST NOTIFICATIONS
// ═══════════════════════════════════════════════════════════════════════════

let toastTimeout = null;

function showToast(msg, type = 'success') {
  const toast = document.getElementById('toast');
  toast.textContent = msg;
  toast.className = `toast toast--${type} toast--visible`;

  clearTimeout(toastTimeout);
  toastTimeout = setTimeout(() => {
    toast.classList.remove('toast--visible');
  }, 2500);
}

// ═══════════════════════════════════════════════════════════════════════════
//  UTILITY
// ═══════════════════════════════════════════════════════════════════════════

function escHtml(str) {
  const div = document.createElement('div');
  div.textContent = str;
  return div.innerHTML;
}

// ═══════════════════════════════════════════════════════════════════════════
//  INIT
// ═══════════════════════════════════════════════════════════════════════════

document.addEventListener('DOMContentLoaded', () => {
  loadLocations();
  renderLocations();
  initCompass();
  initGeolocation();
  startCompassLoop();
  updateStatusBar();

  // Check ESP32 connection and GPS periodically
  pollESP32();
  setInterval(pollESP32, 1000);

  // Update distances periodically
  setInterval(updateDistances, 5000);
});
)rawliteral";

// --- WEB SERVER ROUTES ---

// What to do when the phone connects and navigates to the IP
void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", html_content);
}

void handleCss() {
  server.send_P(200, "text/css; charset=utf-8", css_content);
}

void handleJs() {
  server.send_P(200, "application/javascript; charset=utf-8", js_content);
}



void handleSetDest() {
  if (server.hasArg("lat") && server.hasArg("lon")) {
    TARGET_LAT = server.arg("lat").toDouble();
    TARGET_LON = server.arg("lon").toDouble();
    currentDestinationName = server.hasArg("name") ? server.arg("name") : "Custom";
    
    Serial.print("New destination via Wi-Fi (dynamic): ");
    Serial.print(currentDestinationName);
    Serial.print(" → ");
    Serial.print(TARGET_LAT, 6);
    Serial.print(", ");
    Serial.println(TARGET_LON, 6);
    
    server.send(200, "text/plain", "OK");
    return;
  }
  
  if (server.hasArg("dest")) {
    String dest = server.arg("dest");
    
    if (dest == "1") {
      TARGET_LAT = 53.2895; TARGET_LON = -6.3768;
      currentDestinationName = "Hospital";
    } else if (dest == "2") {
      TARGET_LAT = 53.2875; TARGET_LON = -6.3712;
      currentDestinationName = "Garda Station";
    } else if (dest == "3") {
      TARGET_LAT = 53.3283; TARGET_LON = -6.2305;
      currentDestinationName = "Embassy";
    }
    
    Serial.print("New destination via Wi-Fi (preset): ");
    Serial.println(currentDestinationName);
    
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing lat/lon or dest parameter");
  }
}


// ── Navigation Target ───────────────────────────────────────────────────────
const double RADIUS     = 20.0;    // Destination "arrived" radius in metres

// ── Object Instances ────────────────────────────────────────────────────────
// HardwareSerial(1) = UART1 on the ESP32-S3, mapped to GPS_RX/GPS_TX below.
HardwareSerial GPSSerial(1);

// TinyGPS++ parser object — we feed it raw bytes and it extracts lat/lon/time.
TinyGPSPlus gps;
void handleGps() {
  String json = "{";
  json += "\"valid\":" + String(gps.location.isValid() ? "true" : "false") + ",";
  json += "\"lat\":" + String(gps.location.lat(), 6) + ",";
  json += "\"lon\":" + String(gps.location.lng(), 6) + ",";
  json += "\"heading\":" + String(gps.course.isValid() ? gps.course.deg() : -1.0) + ",";
  json += "\"speed\":" + String(gps.speed.isValid() ? gps.speed.kmph() : 0.0);
  json += "}";
  server.send(200, "application/json", json);
}

void initSoftAP() {
  Serial.println("\n--- Starting Sensatex Wi-Fi Off-Grid ---");
  WiFi.softAP(ssid, password);
  
  IPAddress IP = WiFi.softAPIP();
  Serial.print("Command Centre IP Address: ");
  Serial.println(IP); // Usually prints 192.168.4.1

  // Configure the routes
    server.on("/", handleRoot);
  server.on("/style.css", handleCss);
  server.on("/app.js", handleJs);
  server.on("/gps", handleGps);
  server.on("/set", handleSetDest);
  
  server.begin();
  Serial.println("Web Server activated. Searching for phones!");
}
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
  // Using sin/cos directly:  dx = sin(bearing), dy = -cos(bearing).
  double rad = angleDeg * PI / 180.0; // Angle got by GPS antena data using Satellite HDOP information.
  double dx  =  sin(rad);        // unit vector component along +x
  double dy  = -cos(rad);        // unit vector component along +y (screen)

  // Perpendicular unit vector (90° clockwise on screen) for width offsets.
  double px = -dy;               //  cos(bearing)
  double py =  dx;               //  sin(bearing)

  int tipX = cx + (int)(len * dx);
  int tipY = cy + (int)(len * dy);

  int tailX = cx - (int)(len * dx);
  int tailY = cy - (int)(len * dy);

  // The head occupies 45 % of the total length.
  double headFrac = 0.45;
  int baseX = cx + (int)(len * (1.0 - headFrac * 2.0) * dx);
  int baseY = cy + (int)(len * (1.0 - headFrac * 2.0) * dy);

  int headHalfW = (int)(len * 0.38);
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