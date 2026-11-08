// End-to-end check of the Sensor Lab: drive each virtual sensor and verify
// what the firmware computed (read back from its own screen + raw values).
//   make web && (cd build/web && python3 -m http.server 8000 &) && node tests/lab_smoke.js
// Set SHOTS=dir to save a screenshot of every page.
const { chromium } = require("playwright");
const assert = require("assert/strict");

const URL = (process.env.URL || "http://localhost:8000/") + "lab.html";
const SHOTS = process.env.SHOTS;

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 1200, height: 1000 } });
  const errors = [];
  page.on("pageerror", (e) => errors.push(String(e)));
  page.on("console", (m) => m.type() === "error" && errors.push(m.text()));
  await page.goto(URL);
  await page.waitForTimeout(800);

  const shot = async (name) => SHOTS && page.locator("#screen").screenshot({ path: `${SHOTS}/lab-${name}.png` });
  // Recompute the canvas box on every tap: clicking panel controls scrolls
  // the page, and the device animates when its pose changes.
  const tap = async (x, y) => {
    await page.locator("#screen").scrollIntoViewIfNeeded();
    await page.waitForTimeout(750); // let the 3D pose transition settle
    const box = await page.locator("#screen").boundingBox();
    await page.mouse.click(box.x + (x / 480) * box.width, box.y + (y / 480) * box.height);
  };
  const tab = async (i) => { await tap(40 + i * 80, 22); await page.waitForTimeout(500); };
  const slide = (id, v) => page.$eval("#" + id, (el, v) => {
    el.value = v;
    el.dispatchEvent(new Event("input", { bubbles: true }));
  }, v);
  const raw = (id) => page.locator("#" + id).textContent();
  const nums = async (id) => (await raw(id)).trim().split(/\s+/).map(Number);

  // ---- Level: accelerometer
  await slide("pitch", 12);
  await slide("roll", -20);
  await page.waitForTimeout(400);
  await shot("1-level");
  let [ax, ay, az] = await nums("r-acc");
  assert.ok(Math.abs(ax - -Math.sin(12 * Math.PI / 180)) < 0.03, `ax ${ax}`);
  assert.ok(ay < -0.3, `ay ${ay} (rolled left)`);
  await page.click("#flip");
  await page.waitForTimeout(400);
  [ax, ay, az] = await nums("r-acc");
  assert.ok(az < -0.95, `face down az ${az}`);
  await page.click("#flat");

  // ---- Compass: magnetometers, tilt compensation, interference
  await tab(1);
  await slide("heading", 135);
  await page.waitForTimeout(400);
  await shot("2-compass");
  await slide("pitch", 30);
  await slide("roll", 25);
  await page.waitForTimeout(400);
  await shot("2b-compass-tilted");
  await slide("magnet", 40);
  await page.waitForTimeout(400);
  const magA = await nums("r-magA"), magB = await nums("r-magB");
  const d = Math.hypot(magA[0] - magB[0], magA[1] - magB[1], magA[2] - magB[2]);
  assert.ok(d > 12, `sensors should disagree near a magnet, diff ${d.toFixed(1)}`);
  await shot("2c-compass-magnet");
  await slide("magnet", 0);
  await page.click("#flat");

  // ---- Gyro: spin + shake
  await tab(2);
  await slide("spin", 180);
  await page.waitForTimeout(2200);
  const g = await nums("r-gyro");
  assert.ok(g[2] < -150, `gz ${g[2]} should be ~-180 (clockwise spin)`);
  await page.click("#stop");
  await page.click("#shake");
  // the shake buzz is short (150 ms): wait for it instead of sampling later
  await page.waitForSelector("#device.vibe", { timeout: 1500 });
  await shot("3-gyro");

  // ---- Sound: level + clap
  await tab(3);
  await slide("noise", -38);
  await page.waitForTimeout(1500);
  const mic = parseFloat(await raw("r-mic"));
  assert.ok(Math.abs(mic - -38) < 4, `mic ${mic}`);
  await page.click("#clap");
  await page.waitForTimeout(300);
  await shot("4-sound");

  // ---- Power: fuel gauge
  await tab(4);
  await page.click('#speed [data-k="60"]');
  await page.waitForTimeout(3000);
  const bat = await raw("r-bat");
  const soc = parseFloat(bat), ma = parseFloat(bat.split("V")[1]);
  assert.ok(soc < 82, `battery should drain, soc ${soc}`);
  assert.ok(ma > 40 && ma < 70, `screen-on draw ${ma} mA`);
  await shot("5-power");
  await page.click('#speed [data-k="1"]');

  // ---- I/O: touch, FN button
  await tab(5);
  await tap(300, 150);
  await page.waitForTimeout(200);
  await shot("6-io");
  await page.locator("#fn").dispatchEvent("pointerdown");
  await page.waitForTimeout(300);
  await page.locator("#fn").dispatchEvent("pointerup");
  await page.waitForTimeout(600);
  await shot("7-after-fn"); // FN on the last page wraps to Level

  assert.deepEqual(errors, [], "no JS errors");
  await browser.close();
  console.log("lab smoke test: ok");
})().catch((e) => {
  console.error(e);
  process.exit(1);
});
