// Panel statistik peraga (Rafly). Canvas murni, tanpa library, jalan offline.
//
// Dipakai dari main.js (Bintang):
//   import { initCharts, pushBufferLevel } from "./charts.js";
//   initCharts(document.getElementById("stats-panel"));
// Dari player.js, tiap kali level buffer dihitung (dalam ms audio yang sudah
// terjadwal tetapi belum diputar):
//   pushBufferLevel(ms);
//
// Data: GET /api/stats tiap 1 dtk (kontrak Tugas.md 2.4). Nilai null = tidak
// diketahui, garis terputus. SEMUA ANGKA DI SINI PERAGA, BUKAN BENCHMARK.

const HISTORY = 60;          // titik (= 60 detik)
const POLL_MS = 1000;
const COLORS = ["#2563eb", "#16a34a", "#dc2626", "#d97706"];

const hist = {
  clients: [], streaming: [], ws_sessions: [],
  underruns: [], late_resets: [],
  lateness_ms: [], cpu_pct: [], rss_mb: [],
  buffer_ms: [],
};
let lastBuffer = null;

function push(key, v) {
  const a = hist[key];
  a.push(v === undefined ? null : v);
  if (a.length > HISTORY) a.shift();
}

/** Dipanggil player.js; nilai terakhir diambil tiap detik polling. */
export function pushBufferLevel(ms) { lastBuffer = ms; }

function drawChart(canvas, series, opts = {}) {
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth, h = canvas.clientHeight;
  if (canvas.width !== w * dpr) { canvas.width = w * dpr; canvas.height = h * dpr; }
  const ctx = canvas.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);

  const padL = 38, padR = 8, padT = 8, padB = 18;
  const vals = series.flatMap(s => s.data).filter(v => v !== null);
  let max = Math.max(opts.minMax || 1, ...vals, 1);
  max = Math.ceil(max * 1.1);
  const X = i => padL + (i / (HISTORY - 1)) * (w - padL - padR);
  const Y = v => padT + (1 - v / max) * (h - padT - padB);

  ctx.font = "10px system-ui, sans-serif";
  ctx.fillStyle = ctx.strokeStyle = "#9ca3af";
  ctx.lineWidth = 1;
  for (let k = 0; k <= 2; k++) {                       // grid + label sumbu Y
    const v = (max * k) / 2, y = Y(v);
    ctx.globalAlpha = 0.25; ctx.beginPath(); ctx.moveTo(padL, y); ctx.lineTo(w - padR, y); ctx.stroke();
    ctx.globalAlpha = 1; ctx.textAlign = "right"; ctx.fillText(Number(v.toFixed(1)), padL - 4, y + 3);
  }
  ctx.textAlign = "left"; ctx.fillText("-60 dtk", padL, h - 4);
  ctx.textAlign = "right"; ctx.fillText("sekarang", w - padR, h - 4);

  series.forEach((s, si) => {
    ctx.strokeStyle = COLORS[si % COLORS.length]; ctx.lineWidth = 1.8; ctx.beginPath();
    let pen = false;
    const off = HISTORY - s.data.length;               // rata kanan
    s.data.forEach((v, i) => {
      if (v === null) { pen = false; return; }
      const x = X(i + off), y = Y(v);
      pen ? ctx.lineTo(x, y) : ctx.moveTo(x, y); pen = true;
    });
    ctx.stroke();
  });
}

function panel(parent, title, legend) {
  const box = document.createElement("div");
  box.className = "chart-box";
  box.style.cssText = "flex:1 1 280px;min-width:260px;padding:8px";
  const head = document.createElement("div");
  head.style.cssText = "font:600 13px system-ui;margin-bottom:2px";
  head.textContent = title;
  const lg = document.createElement("div");
  lg.style.cssText = "font:11px system-ui;display:flex;gap:10px;margin-bottom:4px";
  legend.forEach((name, i) => {
    const s = document.createElement("span");
    s.innerHTML = `<b style="color:${COLORS[i]}">&#9632;</b> ${name}`;
    lg.appendChild(s);
  });
  const c = document.createElement("canvas");
  c.style.cssText = "width:100%;height:120px;display:block";
  box.append(head, lg, c);
  parent.appendChild(box);
  return c;
}

/** Bangun panel di `container` dan mulai polling. Mengembalikan fungsi stop(). */
export function initCharts(container) {
  container.innerHTML = "";
  const note = document.createElement("div");
  note.style.cssText = "font:12px system-ui;color:#b45309;margin-bottom:6px";
  note.textContent = "Peraga, bukan data benchmark. Benchmark resmi: klien C dan load_test.py.";
  const row = document.createElement("div");
  row.style.cssText = "display:flex;flex-wrap:wrap;gap:8px";
  const status = document.createElement("div");
  status.style.cssText = "font:12px system-ui;margin-top:6px;color:#6b7280";
  container.append(note, row, status);

  const cv = {
    sessions: panel(row, "Sesi aktif", ["klien server", "streaming", "tab WebSocket"]),
    errors:   panel(row, "Underrun dan late reset (kumulatif)", ["underruns", "late_resets"]),
    late:     panel(row, "Lateness server, maks per detik (ms)", ["max lateness"]),
    res:      panel(row, "CPU % dan RSS (MB) server", ["CPU %", "RSS MB"]),
    buffer:   panel(row, "Level buffer pemutar (ms)", ["buffer"]),
  };

  function redraw() {
    drawChart(cv.sessions, [{ data: hist.clients }, { data: hist.streaming }, { data: hist.ws_sessions }], { minMax: 3 });
    drawChart(cv.errors, [{ data: hist.underruns }, { data: hist.late_resets }]);
    drawChart(cv.late, [{ data: hist.lateness_ms }], { minMax: 2 });
    drawChart(cv.res, [{ data: hist.cpu_pct }, { data: hist.rss_mb }], { minMax: 10 });
    drawChart(cv.buffer, [{ data: hist.buffer_ms }], { minMax: 500 });
  }

  async function tick() {
    try {
      const r = await fetch("/api/stats", { cache: "no-store" });
      const s = await r.json();
      push("clients", s.clients); push("streaming", s.streaming); push("ws_sessions", s.ws_sessions);
      push("underruns", s.underruns); push("late_resets", s.late_resets);
      push("lateness_ms", s.max_lateness_us == null ? null : s.max_lateness_us / 1000);
      push("cpu_pct", s.cpu_pct);
      push("rss_mb", s.rss_kb == null ? null : s.rss_kb / 1024);
      push("buffer_ms", lastBuffer);
      status.textContent = s.t_s == null
        ? "Menunggu statistik server (jalankan server dengan --stats-csv dan gateway dengan --stats-csv)."
        : `t server = ${s.t_s} dtk | chunk terkirim = ${s.chunks_sent}`;
    } catch {
      for (const k of Object.keys(hist)) push(k, null);
      status.textContent = "Gateway tidak terjangkau.";
    }
    redraw();
  }

  tick();
  const id = setInterval(tick, POLL_MS);
  return () => clearInterval(id);
}
