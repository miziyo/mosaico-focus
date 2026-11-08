// Shared by the Focus Cube and Sensor Lab simulator pages.
window.SimCommon = (() => {
  let actx = null;
  let soundOn = () => true;

  function tone(freq, start, dur, gain = 0.18) {
    if (!soundOn()) return;
    try {
      actx = actx || new (window.AudioContext || window.webkitAudioContext)();
    } catch {
      return;
    }
    const t0 = actx.currentTime + start;
    const o = actx.createOscillator();
    const g = actx.createGain();
    o.type = "sine";
    o.frequency.value = freq;
    g.gain.setValueAtTime(0, t0);
    g.gain.linearRampToValueAtTime(gain, t0 + 0.01);
    g.gain.exponentialRampToValueAtTime(0.0001, t0 + dur);
    o.connect(g).connect(actx.destination);
    o.start(t0);
    o.stop(t0 + dur + 0.05);
  }

  // indexes match hal_sound_t
  const SOUNDS = [
    () => tone(880, 0, 0.08, 0.12),                                            // START
    () => { tone(784, 0, 0.5); tone(988, 0.18, 0.5); tone(1319, 0.36, 0.9); }, // DONE
    () => { tone(660, 0, 0.18); tone(660, 0.25, 0.18); },                      // BREAK_OVER
  ];

  function vibrate(device, ms) {
    device.style.setProperty("--vibe-ms", ms + "ms");
    device.classList.remove("vibe");
    void device.offsetWidth; // restart the animation
    device.classList.add("vibe");
    clearTimeout(device._vibeTimer);
    device._vibeTimer = setTimeout(() => device.classList.remove("vibe"), ms);
  }

  // Boot the wasm module, wire the canvas, start the render loop.
  function boot({ canvas, app, sim, onReady }) {
    const ctx = canvas.getContext("2d");
    const img = ctx.createImageData(480, 480);
    let mod = null;

    function pointer(e, down) {
      if (!mod) return;
      const r = canvas.getBoundingClientRect();
      const x = Math.round(((e.clientX - r.left) / r.width) * 480);
      const y = Math.round(((e.clientY - r.top) / r.height) * 480);
      mod._web_pointer(x, y, down ? 1 : 0);
    }
    canvas.addEventListener("pointerdown", (e) => { try { canvas.setPointerCapture(e.pointerId); } catch {} pointer(e, true); });
    canvas.addEventListener("pointermove", (e) => pointer(e, e.buttons > 0));
    canvas.addEventListener("pointerup", (e) => pointer(e, false));
    canvas.addEventListener("pointercancel", (e) => pointer(e, false));

    return createFocusCube({ sim }).then((m) => {
      mod = m;
      m._web_init(app);
      onReady && onReady(m);
      const fbPtr = m._web_framebuffer();
      const px = img.data;
      function frame(t) {
        if (m._web_frame(t)) {
          // LVGL XRGB8888 is B,G,R,X in memory; canvas wants R,G,B,A.
          const src = m.HEAPU8.subarray(fbPtr, fbPtr + 480 * 480 * 4);
          for (let i = 0; i < src.length; i += 4) {
            px[i] = src[i + 2];
            px[i + 1] = src[i + 1];
            px[i + 2] = src[i];
            px[i + 3] = 255;
          }
          ctx.putImageData(img, 0, 0);
        }
        requestAnimationFrame(frame);
      }
      requestAnimationFrame(frame);
      return m;
    });
  }

  return {
    boot,
    vibrate,
    sound: (id) => SOUNDS[id] && SOUNDS[id](),
    setSoundEnabled: (fn) => (soundOn = fn),
  };
})();
