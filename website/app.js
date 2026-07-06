/* ═══════════════════════════════════════════════════════════════════════════
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
    locations = data ? JSON.parse(data) : getDefaultLocations();
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
