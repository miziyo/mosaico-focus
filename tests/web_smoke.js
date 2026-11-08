// End-to-end smoke test of the browser simulator.
//   make web && (cd build/web && python3 -m http.server 8000 &) && node tests/web_smoke.js
// Set SHOTS=dir to also save screenshots of each step.
const { chromium } = require("playwright");
const assert = require("assert/strict");

const URL = process.env.URL || "http://localhost:8000/";
const SHOTS = process.env.SHOTS;

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 1200, height: 800 } });
  const errors = [];
  page.on("pageerror", (e) => errors.push(String(e)));
  page.on("console", (m) => m.type() === "error" && errors.push(m.text()));

  await page.goto(URL);
  await page.evaluate(() => localStorage.clear());
  await page.reload();
  await page.waitForTimeout(1000);

  const shot = async (name) => SHOTS && page.screenshot({ path: `${SHOTS}/${name}.png` });
  const box = await page.locator("#screen").boundingBox();
  const tap = (x, y) => page.mouse.click(box.x + (x / 480) * box.width, box.y + (y / 480) * box.height);
  const text = (id) => page.locator("#" + id).textContent();
  const log = () => page.evaluate(() => JSON.parse(localStorage.getItem("focuscube.log") || "[]"));

  await shot("1-idle");

  // 1. pick the 15 min preset with a quick tap on the device screen
  await tap(140, 408);
  await page.waitForTimeout(300);
  await shot("2-preset-15");

  // 2. full session at 300x: 15 min focus = 3 s real time
  await page.click('[data-k="300"]');
  await page.click('[data-pose="down"]');
  await page.waitForTimeout(800);
  assert.match(await text("r-display"), /꺼짐/, "screen turns off when face down");
  assert.match(await text("r-led"), /빨강/, "LED shows do-not-disturb");
  await shot("3-focus");

  await page.waitForTimeout(3500);
  assert.match(await text("r-led"), /초록/, "LED turns green when target reached");

  await page.click('[data-pose="up"]');
  await page.waitForTimeout(400);
  assert.match(await text("r-led"), /파랑/, "break starts after picking it up");
  await shot("4-break");

  let l = await log();
  assert.equal(l.length, 1, "one session recorded");
  assert.equal(l[0].completed, true);
  assert.equal(l[0].target, 15 * 60000, "15 min preset was applied");

  // 3. skip the break, start again, pick it up midway, end early
  await tap(240, 408); // "Skip break"
  await page.waitForTimeout(300);
  await page.click('[data-pose="down"]');
  await page.waitForTimeout(1500);
  await page.click('[data-pose="tilt"]');
  await page.waitForTimeout(600);
  assert.match(await text("r-display"), /켜짐/, "screen back on when paused");
  await shot("5-paused");
  await tap(240, 408); // "End session"
  await page.waitForTimeout(300);

  l = await log();
  assert.equal(l.length, 2, "interrupted session recorded");
  assert.equal(l[1].completed, false);
  await shot("6-after");

  assert.deepEqual(errors, [], "no JS errors");
  await browser.close();
  console.log("web smoke test: ok");
})().catch((e) => {
  console.error(e);
  process.exit(1);
});
