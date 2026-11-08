// Browser side of the Sensor Lab simulator: drives the device model in main.c.
(() => {
  const $ = (id) => document.getElementById(id);
  const device = $("device");
  let mod = null;

  SimCommon.setSoundEnabled(() => $("sound").checked);

  // ---------- orientation ----------
  function applyOrientation() {
    const pitch = Number($("pitch").value);
    const roll = Number($("roll").value);
    const heading = Number($("heading").value);
    $("pitch-v").textContent = pitch + "°";
    $("roll-v").textContent = roll + "°";
    $("heading-v").textContent = heading + "°";
    device.style.transform = `rotateX(${-pitch}deg) rotateY(${roll}deg)`;
    mod && mod._web_set_orientation(heading, pitch, roll);
  }
  for (const id of ["pitch", "roll", "heading"]) $(id).addEventListener("input", applyOrientation);
  $("flat").onclick = () => { $("pitch").value = 0; $("roll").value = 0; applyOrientation(); };
  $("flip").onclick = () => { $("pitch").value = 0; $("roll").value = 180; applyOrientation(); };
  $("shake").onclick = () => mod && mod._web_shake();

  // ---------- gyro ----------
  function applySpin() {
    const v = Number($("spin").value);
    $("spin-v").textContent = v + "°/s";
    mod && mod._web_set_spin(v);
  }
  $("spin").addEventListener("input", applySpin);
  $("stop").onclick = () => { $("spin").value = 0; applySpin(); };

  // ---------- magnet ----------
  function applyMagnet() {
    const v = Number($("magnet").value);
    $("magnet-v").textContent = v === 0 ? "없음" : v + " µT";
    mod && mod._web_set_magnet(v);
  }
  $("magnet").addEventListener("input", applyMagnet);

  // ---------- sound ----------
  function applyNoise() {
    const db = Number($("noise").value);
    $("noise-v").textContent = db + " dB";
    mod && mod._web_set_noise(Math.pow(10, db / 20));
  }
  $("noise").addEventListener("input", applyNoise);
  $("clap").onclick = () => mod && mod._web_clap();

  // ---------- battery ----------
  let socDragging = false;
  $("soc").addEventListener("pointerdown", () => (socDragging = true));
  window.addEventListener("pointerup", () => (socDragging = false));
  $("soc").addEventListener("input", () => {
    $("soc-v").textContent = $("soc").value + "%";
    mod && mod._web_set_battery(Number($("soc").value), $("charging").checked ? 1 : 0);
  });
  $("charging").addEventListener("change", () => mod && mod._web_set_battery(-1, $("charging").checked ? 1 : 0));
  $("speed").onclick = (e) => {
    const k = Number(e.target.dataset.k);
    if (!k) return;
    for (const b of $("speed").children) b.classList.toggle("on", b === e.target);
    mod && mod._web_set_time_scale(k);
  };

  // ---------- FN button (hold) ----------
  const fn = $("fn");
  const fnSet = (down) => { fn.classList.toggle("down", down); mod && mod._web_set_fn(down ? 1 : 0); };
  fn.addEventListener("pointerdown", (e) => { try { fn.setPointerCapture(e.pointerId); } catch {} fnSet(true); });
  fn.addEventListener("pointerup", () => fnSet(false));
  fn.addEventListener("pointercancel", () => fnSet(false));
  fn.addEventListener("keydown", (e) => { if ((e.key === " " || e.key === "Enter") && !e.repeat) { e.preventDefault(); fnSet(true); } });
  fn.addEventListener("keyup", (e) => { if (e.key === " " || e.key === "Enter") fnSet(false); });

  // ---------- raw readout: what the firmware actually received ----------
  const f = (v, d = 2) => (v >= 0 ? " " : "") + v.toFixed(d);
  function renderReadout() {
    if (!mod) return;
    const r = new Float32Array(mod.HEAPU8.buffer, mod._web_readout(), 16);
    $("r-acc").textContent = `${f(r[0], 3)} ${f(r[1], 3)} ${f(r[2], 3)}`;
    $("r-gyro").textContent = `${f(r[3], 1)} ${f(r[4], 1)} ${f(r[5], 1)}`;
    $("r-magA").textContent = `${f(r[6], 1)} ${f(r[7], 1)} ${f(r[8], 1)}`;
    $("r-magB").textContent = `${f(r[9], 1)} ${f(r[10], 1)} ${f(r[11], 1)}`;
    $("r-mic").textContent = `${r[12].toFixed(1)} dBFS`;
    $("r-bat").textContent = `${r[13].toFixed(1)}%  ${(r[14] / 1000).toFixed(2)} V  ${r[15].toFixed(0)} mA`;

    // follow the model where it moves on its own
    if (!socDragging) {
      $("soc").value = Math.round(r[13]);
      $("soc-v").textContent = Math.round(r[13]) + "%";
    }
    if (Number($("spin").value) !== 0) {
      $("heading").value = Math.round(mod._web_get_heading()) % 360;
      $("heading-v").textContent = $("heading").value + "°";
    }
  }

  // ---------- boot ----------
  SimCommon.boot({
    canvas: $("screen"),
    app: 1,
    sim: {
      sound: SimCommon.sound,
      vibrate: (ms) => SimCommon.vibrate(device, ms),
    },
    onReady: (m) => {
      mod = m;
      applyOrientation();
      applySpin();
      applyMagnet();
      applyNoise();
      mod._web_set_battery(Number($("soc").value), 0);
      setInterval(renderReadout, 200);
    },
  });
})();
