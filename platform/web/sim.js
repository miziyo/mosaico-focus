// Browser side of the Focus Cube simulator: canvas blit, fake IMU, sounds, log.
(() => {
  const POSES = {
    up: [0, 0, 1],
    down: [0, 0, -1],
    tilt: [0.15, 0.75, 0.6], // held in the hand, screen towards you
  };
  const LED_NAMES = ["꺼짐", "빨강 · 방해 금지", "초록 · 완료", "파랑 · 휴식"];
  const LOG_KEY = "focuscube.log";

  const $ = (id) => document.getElementById(id);
  const device = $("device");

  let mod = null;
  let pose = "up";
  let motionOn = false;

  // ---------- storage (never trust it to exist) ----------
  const store = {
    get(k, d) { try { return JSON.parse(localStorage.getItem(k)) ?? d; } catch { return d; } },
    set(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch {} },
  };

  SimCommon.setSoundEnabled(() => $("sound").checked);

  // ---------- session log ----------
  function fmtMin(ms) { return Math.round(ms / 60000) + "분"; }
  function renderLog() {
    const log = store.get(LOG_KEY, []);
    const ol = $("log");
    ol.innerHTML = "";
    if (!log.length) {
      ol.innerHTML = '<li class="empty">아직 기록이 없어요. 기기를 엎어보세요.</li>';
      return;
    }
    for (const r of log.slice().reverse()) {
      const li = document.createElement("li");
      const when = new Date(r.at);
      const hh = String(when.getHours()).padStart(2, "0");
      const mm = String(when.getMinutes()).padStart(2, "0");
      const status = r.completed
        ? `<span class="ok">완료</span>`
        : `<span class="partial">중단</span>`;
      li.innerHTML = `<span>${status} ${fmtMin(r.focused)} / ${fmtMin(r.target)}` +
        (r.pauses ? ` · 일시정지 ${r.pauses}회` : "") + `</span><time>${hh}:${mm}</time>`;
      ol.appendChild(li);
    }
  }
  // Copy instead of download: works everywhere, including sandboxed embeds.
  $("export").onclick = async () => {
    const rows = [["finished_at", "target_min", "focused_min", "pauses", "completed"]];
    for (const r of store.get(LOG_KEY, [])) {
      rows.push([new Date(r.at).toISOString(), (r.target / 60000).toFixed(1),
        (r.focused / 60000).toFixed(1), r.pauses, r.completed]);
    }
    const csv = rows.map((r) => r.join(",")).join("\n");
    const btn = $("export");
    try {
      await navigator.clipboard.writeText(csv);
      btn.textContent = "복사됨";
    } catch {
      console.log(csv);
      btn.textContent = "복사 실패";
    }
    setTimeout(() => (btn.textContent = "CSV 복사"), 1500);
  };
  $("clear").onclick = () => {
    store.set(LOG_KEY, []);
    try { localStorage.removeItem("focuscube.stats"); } catch {}
    location.reload();
  };

  // ---------- HAL callbacks from C ----------
  const sim = {
    sound: SimCommon.sound,
    vibrate: (ms) => SimCommon.vibrate(device, ms),
    led: (id) => {
      $("led").dataset.c = id;
      $("r-led").textContent = LED_NAMES[id] || id;
    },
    awake: (on) => {
      $("veil").classList.toggle("on", !on);
      $("r-display").textContent = on ? "켜짐" : "꺼짐 (배터리 절약)";
    },
    record: (r) => {
      const log = store.get(LOG_KEY, []);
      log.push(r);
      store.set(LOG_KEY, log.slice(-200));
      renderLog();
    },
  };

  // ---------- pose / speed controls ----------
  const CAPTIONS = {
    up: "화면이 위를 보고 있어요",
    down: "엎어둔 상태: 집중 중에는 화면이 꺼져요",
    tilt: "손에 들고 있어요: 집중이 일시정지돼요",
  };
  function setPose(p) {
    pose = p;
    device.className = "device " + (p === "up" ? "" : p);
    $("caption").textContent = CAPTIONS[p];
    for (const b of $("pose").children) b.classList.toggle("on", b.dataset.pose === p);
    if (mod && !motionOn) mod._web_set_accel(...POSES[p]);
  }
  $("pose").onclick = (e) => e.target.dataset.pose && setPose(e.target.dataset.pose);
  document.addEventListener("keydown", (e) => {
    if (e.code === "Space" && e.target === document.body) {
      e.preventDefault();
      setPose(pose === "down" ? "up" : "down");
    }
  });

  $("speed").onclick = (e) => {
    const k = Number(e.target.dataset.k);
    if (!k) return;
    for (const b of $("speed").children) b.classList.toggle("on", b === e.target);
    mod && mod._web_set_time_scale(k);
  };

  // Real phone: gravity vector -> fake IMU. Sign conventions differ by OS,
  // so we only rely on z (face up ~ +1g, face down ~ -1g).
  function onMotion(e) {
    const g = e.accelerationIncludingGravity;
    if (!g || g.z == null || !mod) return;
    const s = /iPhone|iPad/.test(navigator.userAgent) ? -1 : 1;
    const v = [s * g.x / 9.81, s * g.y / 9.81, s * g.z / 9.81];
    mod._web_set_accel(v[0], v[1], v[2]);
    const p = v[2] > 0.8 ? "up" : v[2] < -0.8 ? "down" : "tilt";
    if (p !== pose) { pose = p; $("caption").textContent = CAPTIONS[p]; }
  }
  $("motion").onchange = async (e) => {
    motionOn = e.target.checked;
    if (motionOn) {
      if (typeof DeviceMotionEvent !== "undefined" && DeviceMotionEvent.requestPermission) {
        try { await DeviceMotionEvent.requestPermission(); } catch {}
      }
      window.addEventListener("devicemotion", onMotion);
    } else {
      window.removeEventListener("devicemotion", onMotion);
      setPose(pose);
    }
  };

  // ---------- boot ----------
  SimCommon.boot({
    canvas: $("screen"),
    app: 0,
    sim,
    onReady: (m) => {
      mod = m;
      renderLog();
      setPose("up");
    },
  });
})();
