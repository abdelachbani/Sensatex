# pip install pillow requests
import math, os, requests
from PIL import Image
from io import BytesIO
from time import sleep

ZOOM = 14
OUTPUT_DIR = "maps"
TILE_SIZE = 256

def lat_lon_to_tile(lat, lon, zoom):
    n = 2 ** zoom
    x = int((lon + 180) / 360 * n)
    lat_rad = math.radians(lat)
    y = int((1 - math.log(math.tan(lat_rad) + 1 / math.cos(lat_rad)) / math.pi) / 2 * n)
    return x, y

def tile_to_lat(y, zoom):
    n = 2 ** zoom
    lat_rad = math.atan(math.sinh(math.pi * (1 - 2 * y / n)))
    return math.degrees(lat_rad)

def tile_to_lon(x, zoom):
    return x / (2 ** zoom) * 360 - 180

def adjust_proportion(lat_min, lat_max, lon_min, lon_max, proportion=(3, 2)):
    """Adjusts the bounding box to have the specified aspect ratio (width:height)"""
    lat_center = (lat_min + lat_max) / 2
    lon_center = (lon_min + lon_max) / 2

    lat_span = lat_max - lat_min
    lon_span = lon_max - lon_min

    target_lon = lat_span * (proportion[0] / proportion[1])
    target_lat = lon_span * (proportion[1] / proportion[0])

    # Expand the smaller axis to reach the target ratio
    if lon_span / lat_span < proportion[0] / proportion[1]:
        # Too tall -> widen longitude
        lon_min = lon_center - target_lon / 2
        lon_max = lon_center + target_lon / 2
    else:
        # Too wide -> widen latitude
        lat_min = lat_center - target_lat / 2
        lat_max = lat_center + target_lat / 2

    return lat_min, lat_max, lon_min, lon_max

def download_tile(x, y, zoom, server="a"):
    url = f"https://{server}.tile.openstreetmap.org/{zoom}/{x}/{y}.png"
    headers = {"User-Agent": "MapGenerator/1.0"}
    try:
        r = requests.get(url, headers=headers, timeout=10)
        if r.status_code == 200:
            return Image.open(BytesIO(r.content)).convert("RGB")
    except Exception as e:
        print(f"  ⚠️  Tile error {x},{y}: {e}")
    return Image.new("RGB", (TILE_SIZE, TILE_SIZE), (200, 200, 200))

def generate_map(region, zoom, proportion=(3, 2)):
    name = region["name"]
    print(f"\n📍 Generating {name} (zoom={zoom}, proportion {proportion[0]}:{proportion[1]})...")

    # Adjust proportion
    lat_min, lat_max, lon_min, lon_max = adjust_proportion(
        region["lat_min"], region["lat_max"],
        region["lon_min"], region["lon_max"],
        proportion
    )

    x_min, y_max = lat_lon_to_tile(lat_min, lon_min, zoom)
    x_max, y_min = lat_lon_to_tile(lat_max, lon_max, zoom)

    x_min, x_max = min(x_min, x_max), max(x_min, x_max)
    y_min, y_max = min(y_min, y_max), max(y_min, y_max)

    cols = x_max - x_min + 1
    rows = y_max - y_min + 1
    total = cols * rows
    px_w = cols * TILE_SIZE
    px_h = rows * TILE_SIZE
    print(f"   Tiles: {cols}×{rows} = {total} → image {px_w}×{px_h}px  (actual ratio {px_w/px_h:.2f}:1)")

    img = Image.new("RGB", (px_w, px_h))
    servers = ["a", "b", "c"]
    count = 0
    for row, y in enumerate(range(y_min, y_max + 1)):
        for col, x in enumerate(range(x_min, x_max + 1)):
            tile = download_tile(x, y, zoom, servers[count % 3])
            img.paste(tile, (col * TILE_SIZE, row * TILE_SIZE))
            count += 1
            if count % 10 == 0:
                print(f"   {count}/{total} tiles...", end="\r")
            sleep(0.05)

    os.makedirs(OUTPUT_DIR, exist_ok=True)
    path = os.path.join(OUTPUT_DIR, f"{name}.jpg")
    img.save(path, "JPEG", quality=95)
    size_mb = os.path.getsize(path) / 1024 / 1024
    print(f"   ✅ {path}  ({px_w}×{px_h}px, {size_mb:.1f} MB)")

    # Real coordinates of the generated image
    real_lat_max = tile_to_lat(y_min, zoom)
    real_lat_min = tile_to_lat(y_max + 1, zoom)
    real_lon_min = tile_to_lon(x_min, zoom)
    real_lon_max = tile_to_lon(x_max + 1, zoom)

    print(f"\n   📋 Copy this into ensureDefaultMaps():")
    print(f'   new MapRegion("{name}", "maps/{name}.jpg",')
    print(f'       {real_lat_min:.6f}, {real_lat_max:.6f},')
    print(f'       {real_lon_min:.6f}, {real_lon_max:.6f});')

    return real_lat_min, real_lat_max, real_lon_min, real_lon_max


REGIONS = [
    # {
    #     "name":  "valencia",
    #     "lat_min": 39.35, "lat_max": 39.55,
    #     "lon_min": -0.50, "lon_max": -0.30,
    # },
    # {
    #     "name":  "calderona",
    #     "lat_min": 39.60, "lat_max": 39.75,
    #     "lon_min": -0.55, "lon_max": -0.30,
    # },
    # {
    #     "name":  "pyrenees",
    #     "lat_min": 42.60, "lat_max": 42.80,
    #     "lon_min":  0.00, "lon_max":  0.30,
    # },
    {
        "name":  "test",
        "lat_min": 39.35, "lat_max": 39.55,
        "lon_min": -0.50, "lon_max": -0.30,
     },
]

if __name__ == "__main__":
    print(f"🗺️  HD map generator — Zoom {ZOOM} — Proportion 3:2")
    for region in REGIONS:
        generate_map(region, ZOOM, proportion=(3, 2))
    print("\n🎉 Done!")
