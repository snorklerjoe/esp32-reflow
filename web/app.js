'use strict';
/* Reflow Oven Controller web app. Talks to the firmware's JSON API (see README). */

const $ = (s, el = document) => el.querySelector(s);
const $$ = (s, el = document) => Array.from(el.querySelectorAll(s));

const COLOR = { amb: '#38bdf8', aux: '#ff7a1a', prof: '#c084fc', sp: '#facc15', duty: '#ff3d7f' };
const LEGEND = {
  amb: ['Ambient', COLOR.amb], aux: ['Aux', COLOR.aux], sp: ['Setpoint', COLOR.sp, true],
  prof: ['Profile', COLOR.prof, true], duty: ['Heater duty', 'rgba(255,61,127,.55)'],
};
const MODE_LABEL = { idle: 'Idle', manual: 'Manual', hold: 'Holding', reflow: 'Reflow', autotune: 'Autotune' };
const FAULT_TEXT = { open: 'Thermocouple open / disconnected', nodev: 'No MAX6675 detected' };
const HIST_MAX = 3600;

const S = {
  role: 'none', info: {}, status: null,
  hist: [], histNext: 0, histBusy: false,
  profiles: [], selProfile: null, rfDismissed: false,
  settings: null, defaults: null, rules: null,
  lastEvent: null, pollTimer: null, tab: 'home', charts: {}, upload: null,
};

/* ================================================================== utils */

const isNum = v => typeof v === 'number' && isFinite(v);
const fmt1 = v => isNum(v) ? v.toFixed(1) : '--.-';
const fmtG = v => !isNum(v) ? '–' : Math.abs(v) >= 100 ? v.toFixed(1) : Math.abs(v) >= 1 ? v.toFixed(3) : v.toPrecision(3);
function fmtDur(s) {
  if (!isNum(s)) return '--:--';
  const neg = s < 0; s = Math.abs(Math.round(s));
  return (neg ? '-' : '') + Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');
}
const esc = s => String(s ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
function hexA(hex, a) {
  const n = parseInt(hex.slice(1), 16);
  return `rgba(${n >> 16},${(n >> 8) & 255},${n & 255},${a})`;
}

function toast(text, level = 'info') {
  const el = document.createElement('div');
  el.className = 'toast ' + level;
  el.textContent = text;
  $('#toasts').append(el);
  setTimeout(() => el.classList.add('out'), 4200);
  setTimeout(() => el.remove(), 4700);
}

function shake(el) { el.classList.remove('shake'); void el.offsetWidth; el.classList.add('shake'); }

/* ==================================================================== api */

class ApiError extends Error { constructor(status, msg) { super(msg); this.status = status; } }

async function api(path, { method = 'GET', body } = {}) {
  const opts = { method, headers: {}, credentials: 'same-origin' };
  if (body !== undefined) { opts.headers['Content-Type'] = 'application/json'; opts.body = JSON.stringify(body); }
  let res;
  try { res = await fetch(path, opts); } catch { throw new ApiError(0, 'Connection lost'); }
  let data = null;
  try { data = await res.json(); } catch { /* empty body */ }
  if (res.status === 401 && path !== '/api/login') showLogin();
  if (!res.ok) throw new ApiError(res.status, (data && data.error) || `HTTP ${res.status}`);
  return data;
}

// Runs fn with btn disabled; reports errors as toasts.
async function act(btn, fn) {
  if (btn) btn.disabled = true;
  try { return await fn(); } catch (e) { if (e.status !== 401) toast(e.message, 'error'); } finally { if (btn) btn.disabled = false; }
}

/* ================================================================= charts */

function niceStep(raw) {
  const p = Math.pow(10, Math.floor(Math.log10(raw))), f = raw / p;
  return (f <= 1 ? 1 : f <= 2 ? 2 : f <= 2.5 ? 2.5 : f <= 5 ? 5 : 10) * p;
}
function timeStep(span) {
  for (const s of [5, 10, 15, 30, 60, 120, 180, 300, 600, 900, 1200, 1800, 3600]) if (span / s <= 8) return s;
  return 7200;
}
// Index of the point in sorted data whose x is closest to xv.
function nearest(data, xv) {
  let lo = 0, hi = data.length - 1;
  if (hi < 0) return -1;
  while (lo < hi) { const m = (lo + hi) >> 1; if (data[m][0] < xv) lo = m + 1; else hi = m; }
  if (lo > 0 && Math.abs(data[lo - 1][0] - xv) < Math.abs(data[lo][0] - xv)) lo--;
  return lo;
}

class LineChart {
  constructor(canvas) {
    this.cv = canvas; this.ctx = canvas.getContext('2d'); this.series = []; this.view = {}; this.hx = null;
    new ResizeObserver(() => this.draw()).observe(canvas);
    canvas.addEventListener('pointermove', e => { this.hx = e.offsetX; this.draw(); });
    canvas.addEventListener('pointerleave', () => { this.hx = null; this.draw(); });
  }
  set(series, view = {}) { this.series = series; this.view = view; this.draw(); }
  draw() {
    const cv = this.cv, w = cv.clientWidth, h = cv.clientHeight;
    if (!w || !h) return;
    const dpr = window.devicePixelRatio || 1;
    if (cv.width !== Math.round(w * dpr) || cv.height !== Math.round(h * dpr)) {
      cv.width = Math.round(w * dpr); cv.height = Math.round(h * dpr);
    }
    const ctx = this.ctx, v = this.view;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);

    // ---- bounds
    let x0 = Infinity, x1 = -Infinity, y0 = Infinity, y1 = -Infinity;
    for (const s of this.series) for (const [x, y] of s.data) {
      if (x < x0) x0 = x; if (x > x1) x1 = x;
      if (!s.duty && y != null) { if (y < y0) y0 = y; if (y > y1) y1 = y; }
    }
    if (v.xMin != null) x0 = v.xMin;
    if (v.xMax != null) x1 = v.xMax;
    if (!isFinite(x0) || !isFinite(x1)) { x0 = 0; x1 = 300; }
    if (x1 - x0 < 30) x1 = x0 + 30;
    if (!isFinite(y0)) { y0 = 20; y1 = 100; }
    const pad = Math.max(4, (y1 - y0) * 0.08);
    y0 -= pad; y1 += pad;
    const yStep = niceStep((y1 - y0) / 5);
    y0 = Math.floor(y0 / yStep) * yStep; y1 = Math.ceil(y1 / yStep) * yStep;

    const P = { l: 44, r: 12, t: 12, b: 24 }, pw = w - P.l - P.r, ph = h - P.t - P.b;
    const X = x => P.l + (x - x0) / (x1 - x0) * pw;
    const Y = y => P.t + (1 - (y - y0) / (y1 - y0)) * ph;
    const bottom = P.t + ph;

    // ---- grid
    ctx.font = '11px ui-monospace, Menlo, Consolas, monospace';
    ctx.lineWidth = 1; ctx.strokeStyle = 'rgba(255,255,255,.07)'; ctx.fillStyle = '#7d83a8';
    ctx.textAlign = 'right'; ctx.textBaseline = 'middle';
    for (let y = y0; y <= y1 + 1e-6; y += yStep) {
      const py = Math.round(Y(y)) + .5;
      ctx.beginPath(); ctx.moveTo(P.l, py); ctx.lineTo(w - P.r, py); ctx.stroke();
      ctx.fillText(Math.round(y) + '°', P.l - 6, py);
    }
    const xStep = timeStep(x1 - x0);
    ctx.textAlign = 'center'; ctx.textBaseline = 'top';
    for (let x = Math.ceil(x0 / xStep) * xStep; x <= x1 + 1e-6; x += xStep) {
      const px = Math.round(X(x)) + .5;
      ctx.beginPath(); ctx.moveTo(px, P.t); ctx.lineTo(px, bottom); ctx.stroke();
      ctx.fillText(fmtDur(x), px, bottom + 7);
    }

    ctx.save();
    ctx.beginPath(); ctx.rect(P.l, P.t, pw, ph); ctx.clip();

    // ---- heater duty as a faint band along the bottom (0-100% -> lower 30%)
    for (const s of this.series.filter(s => s.duty && s.data.length)) {
      const g = ctx.createLinearGradient(0, bottom - ph * .3, 0, bottom);
      g.addColorStop(0, 'rgba(255,61,127,.40)'); g.addColorStop(1, 'rgba(255,61,127,.04)');
      ctx.fillStyle = g;
      ctx.beginPath(); ctx.moveTo(X(s.data[0][0]), bottom);
      for (const [x, d] of s.data) ctx.lineTo(X(x), bottom - (d || 0) / 100 * ph * .3);
      ctx.lineTo(X(s.data[s.data.length - 1][0]), bottom); ctx.closePath(); ctx.fill();
    }

    // ---- lines
    for (const s of this.series.filter(s => !s.duty)) {
      const pts = s.data.filter(p => p[1] != null);
      if (!pts.length) continue;
      if (s.fill) {
        const g = ctx.createLinearGradient(0, P.t, 0, bottom);
        g.addColorStop(0, hexA(s.color, .28)); g.addColorStop(1, hexA(s.color, 0));
        ctx.fillStyle = g;
        ctx.beginPath(); ctx.moveTo(X(pts[0][0]), bottom);
        for (const [x, y] of pts) ctx.lineTo(X(x), Y(y));
        ctx.lineTo(X(pts[pts.length - 1][0]), bottom); ctx.closePath(); ctx.fill();
      }
      ctx.save();
      ctx.strokeStyle = s.color; ctx.lineWidth = s.width || 2.2; ctx.lineJoin = 'round'; ctx.lineCap = 'round';
      ctx.setLineDash(s.dash || []);
      ctx.shadowColor = s.color; ctx.shadowBlur = s.glow ?? 10;
      ctx.beginPath();
      let pen = false;
      for (const [x, y] of s.data) {
        if (y == null) { pen = false; continue; }
        pen ? ctx.lineTo(X(x), Y(y)) : ctx.moveTo(X(x), Y(y));
        pen = true;
      }
      ctx.stroke();
      ctx.restore();
    }

    // ---- "now" marker
    if (v.marker != null) {
      const px = X(v.marker);
      ctx.save();
      ctx.strokeStyle = 'rgba(255,255,255,.55)'; ctx.setLineDash([3, 4]);
      ctx.beginPath(); ctx.moveTo(px, P.t); ctx.lineTo(px, bottom); ctx.stroke();
      ctx.restore();
    }
    ctx.restore();

    // ---- hover tooltip
    if (this.hx == null || this.hx < P.l || this.hx > w - P.r) return;
    const xv = x0 + (this.hx - P.l) / pw * (x1 - x0);
    const rows = [];
    for (const s of this.series) {
      const i = nearest(s.data, xv);
      if (i < 0) continue;
      const [x, y] = s.data[i];
      if (y == null || Math.abs(X(x) - this.hx) > 24) continue;
      rows.push([s.name, s.duty ? Math.round(y) + '%' : y.toFixed(1) + '°C', s.duty ? COLOR.duty : s.color]);
      if (!s.duty) {
        ctx.fillStyle = s.color; ctx.beginPath(); ctx.arc(X(x), Y(y), 4, 0, Math.PI * 2); ctx.fill();
        ctx.strokeStyle = '#07080f'; ctx.lineWidth = 2; ctx.stroke();
      }
    }
    ctx.strokeStyle = 'rgba(255,255,255,.25)'; ctx.lineWidth = 1; ctx.setLineDash([]);
    ctx.beginPath(); ctx.moveTo(this.hx + .5, P.t); ctx.lineTo(this.hx + .5, bottom); ctx.stroke();
    if (!rows.length) return;
    ctx.font = '12px ui-monospace, Menlo, Consolas, monospace';
    const title = fmtDur(xv);
    const bw = Math.max(...rows.map(r => ctx.measureText(r[0] + '  ' + r[1]).width), ctx.measureText(title).width) + 34;
    const bh = 26 + rows.length * 18;
    let bx = this.hx + 14; if (bx + bw > w - 4) bx = this.hx - 14 - bw;
    const by = P.t + 6;
    ctx.fillStyle = 'rgba(10,12,26,.94)'; ctx.strokeStyle = 'rgba(255,255,255,.14)';
    ctx.beginPath(); ctx.roundRect ? ctx.roundRect(bx, by, bw, bh, 6) : ctx.rect(bx, by, bw, bh); ctx.fill(); ctx.stroke();
    ctx.textAlign = 'left'; ctx.textBaseline = 'middle';
    ctx.fillStyle = '#8a8fb3'; ctx.fillText(title, bx + 10, by + 13);
    rows.forEach(([name, val, color], i) => {
      const ry = by + 31 + i * 18;
      ctx.fillStyle = color; ctx.fillRect(bx + 10, ry - 4, 8, 8);
      ctx.fillStyle = '#eef0ff'; ctx.fillText(name, bx + 24, ry);
      ctx.textAlign = 'right'; ctx.fillText(val, bx + bw - 10, ry); ctx.textAlign = 'left';
    });
  }
}

const chart = id => S.charts[id] || (S.charts[id] = new LineChart($('#' + id)));

function drawSpark(canvas, pts) {
  const dpr = window.devicePixelRatio || 1, w = canvas.clientWidth, h = canvas.clientHeight;
  canvas.width = w * dpr; canvas.height = h * dpr;
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  if (pts.length < 2) return;
  const t1 = pts[pts.length - 1][0], c1 = Math.max(...pts.map(p => p[1]));
  const g = ctx.createLinearGradient(0, 0, w, 0);
  g.addColorStop(0, '#ff3d7f'); g.addColorStop(1, '#ffb800');
  ctx.strokeStyle = g; ctx.lineWidth = 2; ctx.lineJoin = 'round';
  ctx.beginPath();
  pts.forEach(([t, c], i) => {
    const x = 2 + t / t1 * (w - 4), y = h - 2 - c / c1 * (h - 4);
    i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
  });
  ctx.stroke();
}

function renderLegends() {
  $$('[data-legend]').forEach(el => {
    el.innerHTML = el.dataset.legend.split(' ').map(k => {
      const [name, color, dash] = LEGEND[k];
      return `<span style="color:${color}"><i class="${dash ? 'dash' : ''}" style="background:${color}"></i><span style="color:var(--muted)">${name}</span></span>`;
    }).join('');
  });
}

/* ================================================================ history */

async function syncHistory() {
  const st = S.status;
  if (!st || S.histBusy) return;
  if (st.seq < S.histNext) { S.hist = []; S.histNext = 0; } // controller rebooted
  if (S.histNext >= st.seq) return;
  S.histBusy = true;
  try {
    let since = Math.max(S.histNext, st.seq - HIST_MAX);
    while (since < st.seq) {
      const d = await api(`/api/history?since=${since}`);
      if (S.hist.length && d.from !== S.histNext) S.hist = []; // gap: start over
      d.s.forEach(([amb, aux, sp, duty], i) => S.hist.push({ seq: d.from + i, amb, aux, sp, duty }));
      S.histNext = since = d.next;
      if (!d.s.length) break;
    }
    if (S.hist.length > HIST_MAX) S.hist.splice(0, S.hist.length - HIST_MAX);
  } finally { S.histBusy = false; }
}

function histRange(fromSeq, toSeq) {
  return S.hist.filter(h => h.seq >= fromSeq && (toSeq == null || h.seq < toSeq));
}

// Chart series from history samples, x = seconds relative to x0Seq.
function histSeries(samples, x0Seq, { sp = true, duty = true } = {}) {
  const amb = [], aux = [], spv = [], du = [];
  for (const h of samples) {
    const x = h.seq - x0Seq;
    amb.push([x, h.amb]); aux.push([x, h.aux]); spv.push([x, h.sp]); du.push([x, h.duty]);
  }
  const out = [];
  if (duty) out.push({ name: 'Heater', color: COLOR.duty, data: du, duty: true });
  if (sp) out.push({ name: 'Setpoint', color: COLOR.sp, data: spv, dash: [6, 5], width: 1.6, glow: 0 });
  out.push({ name: 'Ambient', color: COLOR.amb, data: amb }, { name: 'Aux', color: COLOR.aux, data: aux });
  return out;
}

/* ================================================================ polling */

async function poll() {
  clearTimeout(S.pollTimer);
  if (S.role === 'none') return;
  try {
    const st = await api('/api/status');
    S.status = st;
    if (st.role && st.role !== S.role) { S.role = st.role; updateRoleUi(); }
    $('#hdr-conn').classList.remove('down');
    await syncHistory();
    handleEvent(st.event);
    render();
  } catch (e) {
    if (e.status !== 401) $('#hdr-conn').classList.add('down');
  } finally {
    if (S.role !== 'none') S.pollTimer = setTimeout(poll, 1000);
  }
}

function handleEvent(ev) {
  if (!ev) return;
  if (S.lastEvent !== null && ev.id !== S.lastEvent && ev.text) toast(ev.text, ev.level);
  S.lastEvent = ev.id;
}

/* ================================================================= render */

function render() {
  if (!S.status) return;
  renderHeader();
  ({ home: renderHome, hold: renderHold, reflow: renderReflow, autotune: renderAutotune }[S.tab] || (() => {}))();
}

function renderHeader() {
  const st = S.status;
  const m = $('#hdr-mode');
  m.textContent = MODE_LABEL[st.mode] || st.mode; m.dataset.mode = st.mode;
  $('#hdr-amb').textContent = fmt1(st.sensors.ambient.c) + '°';
  $('#hdr-aux').textContent = fmt1(st.sensors.aux.c) + '°';
  $('#hdr-heat').classList.toggle('on', st.heater);
  $('#stop-all').classList.toggle('hidden', st.mode === 'idle');
  const door = !!(st.run && st.run.active && st.run.openDoor);
  $('#door').classList.toggle('show', door);
}

function tempTile(key, s) {
  const tile = $(`.tcard.${key}`);
  const ok = s.fault === 'ok';
  tile.classList.toggle('fault', !ok);
  $(`#t-${key}`).textContent = ok ? fmt1(s.c) : 'ERR';
  $(`#t-${key}-f`).textContent = ok ? (key === 'amb' ? 'Ambient thermocouple' : 'Aux thermocouple') : FAULT_TEXT[s.fault];
}

function renderHome() {
  const st = S.status;
  tempTile('amb', st.sensors.ambient);
  tempTile('aux', st.sensors.aux);
  const coil = $('#h-coil');
  coil.classList.toggle('on', st.heater);
  coil.classList.toggle('warm', !st.heater && st.duty > 0);
  $('#h-state').textContent = st.mode === 'idle' ? 'OFF' : st.mode === 'manual' ? 'ON' : 'AUTO';
  $('#h-sub').textContent = st.mode === 'idle' ? 'Idle'
    : `${MODE_LABEL[st.mode]} · ${Math.round(st.duty)}% duty · SSR ${st.heater ? 'on' : 'off'}`;
  $('#h-on').classList.toggle('active', st.mode === 'manual');

  const last = S.hist.slice(-600);
  const end = last.length ? last[last.length - 1].seq : 0;
  chart('c-home').set(histSeries(last, end, { sp: false }), { xMin: -600, xMax: 0 });
}

function renderHold() {
  const st = S.status, active = st.mode === 'hold';
  const pv = active ? st.sensors[st.sensor].c : null;
  $('#hold-v-sp').textContent = active ? fmt1(st.setpoint) : '--.-';
  $('#hold-v-pv').textContent = fmt1(pv);
  $('#hold-v-err').textContent = active && isNum(pv) ? (st.setpoint - pv >= 0 ? '+' : '') + (st.setpoint - pv).toFixed(1) : '--.-';
  $('#hold-v-duty').textContent = Math.round(st.duty) + '%';
  $('#hold-meter').style.width = (active ? st.duty : 0) + '%';
  const pid = st.pid || { p: 0, i: 0, d: 0 };
  $('#hold-p').textContent = pid.p.toFixed(1); $('#hold-i').textContent = pid.i.toFixed(1); $('#hold-d').textContent = pid.d.toFixed(1);
  $('#hold-start').textContent = active ? 'Update setpoint' : 'Start hold';
  $('#hold-start').disabled = st.mode === 'reflow' || st.mode === 'autotune';

  const run = st.run && st.run.kind === 'hold' ? st.run : null;
  if (run) {
    $('#hold-chart-t').textContent = run.active ? 'Hold run · ' + fmtDur(run.elapsed) : 'Last hold run · ' + run.result;
    chart('c-hold').set(histSeries(histRange(run.startSeq, run.endSeq), run.startSeq), { xMin: 0 });
  } else {
    $('#hold-chart-t').textContent = 'Hold run';
    chart('c-hold').set([], {});
  }
}

function phaseOf(pts, t) {
  if (!pts || pts.length < 2) return 'Ready';
  if (t >= pts[pts.length - 1][0]) return 'Complete';
  let i = pts.findIndex(p => p[0] > t);
  if (i < 1) i = 1;
  const [ta, ca] = pts[i - 1], [tb, cb] = pts[i];
  const slope = (cb - ca) / (tb - ta), sp = ca + slope * (t - ta);
  const peak = Math.max(...pts.map(p => p[1]));
  if (slope < -0.05) return 'Cooling';
  if (sp >= peak - 20) return 'Reflow';
  if (slope > 0.7) return 'Ramp';
  return 'Soak';
}

function renderReflow() {
  const st = S.status, active = st.mode === 'reflow';
  const run = st.run && st.run.kind === 'reflow' ? st.run : null;
  const showRun = run && (run.active || !S.rfDismissed);
  const busy = st.mode !== 'idle' && st.mode !== 'manual';

  $('#rf-start').classList.toggle('hidden', active);
  $('#rf-stop').classList.toggle('hidden', !active);
  $('#rf-start').disabled = busy || !S.profiles.length;
  $('#rf-profile').disabled = active;
  $('.seg[data-name="rf-sensor"]').classList.toggle('disabled', active);

  const pid = showRun ? run.profileId : +$('#rf-profile').value;
  const prof = S.profiles.find(p => p.id === pid);
  const pts = prof ? prof.points : [];
  const duration = showRun ? run.duration : (prof ? prof.duration : 0);
  const elapsed = showRun ? run.elapsed : 0;

  const phase = $('#rf-phase');
  if (active) phase.textContent = phaseOf(pts, elapsed);
  else if (showRun) phase.textContent = run.result === 'complete' ? 'Complete ✓' : run.result === 'failed' ? 'Failed' : 'Aborted';
  else phase.textContent = 'Ready';
  phase.classList.toggle('idle', !active);
  $('#rf-elapsed').textContent = fmtDur(elapsed);
  $('#rf-total').textContent = fmtDur(duration);
  $('#rf-progress').style.width = (duration ? Math.min(100, elapsed / duration * 100) : 0) + '%';
  $('#rf-v-sp').textContent = active ? fmt1(st.setpoint) : '--.-';
  $('#rf-v-aux').textContent = fmt1(st.sensors.aux.c);
  $('#rf-v-amb').textContent = fmt1(st.sensors.ambient.c);
  $('#rf-v-duty').textContent = Math.round(st.duty) + '%';
  $('#rf-chart-t').textContent = showRun ? `${run.profileName} · ${run.sensor} reference` : (prof ? prof.name : 'Profile');

  const series = [{ name: 'Profile', color: COLOR.prof, data: pts, dash: [7, 5], width: 2, glow: 6, fill: true }];
  let xMax = duration;
  if (showRun) {
    const samples = histRange(run.startSeq, run.endSeq);
    series.push(...histSeries(samples, run.startSeq, { sp: false }));
    if (samples.length) xMax = Math.max(xMax, samples[samples.length - 1].seq - run.startSeq);
  }
  chart('c-reflow').set(series, { xMin: 0, xMax: Math.max(xMax, 60), marker: active ? elapsed : null });
}

function renderAutotune() {
  const st = S.status, at = st.autotune, active = st.mode === 'autotune';
  const run = st.run && st.run.kind === 'autotune' ? st.run : null;
  const phaseText = { heating: 'Heating to target', settling: 'Settling', cycling: 'Measuring' };
  $('#at-state').textContent = active ? phaseText[at.phase] || at.phase
    : at && at.result ? 'Complete' : run ? (run.result === 'failed' ? 'Failed' : 'Stopped') : 'Idle';
  $('#at-cyc').textContent = at ? `${at.measured}/${at.cycles}` : '–';
  $('#at-per').textContent = at && isNum(at.lastPeriod) ? fmtDur(at.lastPeriod) : '–';
  $('#at-amp').textContent = at && isNum(at.lastAmplitude) ? '±' + at.lastAmplitude.toFixed(1) + '°' : '–';
  $('#at-start').disabled = active || st.mode === 'reflow' || st.mode === 'hold';
  $('#at-kp').textContent = fmtG(st.gains.kp); $('#at-ki').textContent = fmtG(st.gains.ki); $('#at-kd').textContent = fmtG(st.gains.kd);

  const res = at && at.result;
  $('#at-result').classList.toggle('hidden', !res);
  if (res) {
    $('#at-ku').textContent = res.ku.toFixed(2);
    $('#at-tu').textContent = res.tu.toFixed(0) + ' s';
    const html = res.rules.map(r => `<tr><td>${esc(r.name)}</td><td>${fmtG(r.kp)}</td><td>${fmtG(r.ki)}</td><td>${fmtG(r.kd)}</td>
      <td><button class="btn sm ${r.id === 'none' ? 'hot' : ''}" data-rule="${r.id}">Apply</button></td></tr>`).join('');
    const body = $('#at-rules');
    if (body.dataset.html !== html) { body.innerHTML = html; body.dataset.html = html; }
  }
  if (run) chart('c-autotune').set(histSeries(histRange(run.startSeq, run.endSeq), run.startSeq), { xMin: 0 });
  else chart('c-autotune').set([], {});
}

/* =============================================================== profiles */

async function loadProfiles() {
  const d = await api('/api/profiles');
  S.profiles = d.profiles;
  $('#pf-count').textContent = `${S.profiles.length} / ${d.max}`;
  if (!S.profiles.some(p => p.id === S.selProfile)) S.selProfile = S.profiles.length ? S.profiles[0].id : null;
  renderProfileList();
  renderProfileDetail();
  fillReflowSelect();
}

function renderProfileList() {
  const ul = $('#pf-list');
  if (!S.profiles.length) { ul.innerHTML = '<li class="empty">No profiles yet. Upload a CSV below.</li>'; return; }
  ul.innerHTML = S.profiles.map(p => `<li data-id="${p.id}" class="${p.id === S.selProfile ? 'on' : ''}">
      <canvas></canvas><div><b>${esc(p.name)}</b><small>${fmtDur(p.duration)} · ${Math.round(p.peak)}°C peak · ${p.points.length} pts</small></div></li>`).join('');
  $$('li[data-id]', ul).forEach(li => drawSpark($('canvas', li), S.profiles.find(p => p.id === +li.dataset.id).points));
}

function profileStats(pts) {
  let up = 0, down = 0;
  for (let i = 1; i < pts.length; i++) {
    const s = (pts[i][1] - pts[i - 1][1]) / (pts[i][0] - pts[i - 1][0]);
    up = Math.max(up, s); down = Math.min(down, s);
  }
  return { up, down };
}

function renderProfileDetail() {
  const p = S.profiles.find(p => p.id === S.selProfile);
  $('#pf-empty').classList.toggle('hidden', !!p);
  $('#pf-body').classList.toggle('hidden', !p);
  if (!p) return;
  $('#pf-name').value = p.name;
  $('#pf-desc').value = p.description;
  const { up, down } = profileStats(p.points);
  $('#pf-stats').innerHTML = [
    ['Duration', fmtDur(p.duration)], ['Peak', Math.round(p.peak) + '°C'], ['Points', p.points.length],
    ['Max ramp', up.toFixed(2) + '°C/s'], ['Max cool', down.toFixed(2) + '°C/s'],
  ].map(([k, v]) => `<span>${k} <b>${v}</b></span>`).join('');
  requestAnimationFrame(() => chart('c-profile').set(
    [{ name: 'Profile', color: COLOR.prof, data: p.points, fill: true, width: 2.4 }], { xMin: 0 }));
}

function fillReflowSelect() {
  const sel = $('#rf-profile'), prev = +sel.value;
  sel.innerHTML = S.profiles.map(p => `<option value="${p.id}">${esc(p.name)} (${fmtDur(p.duration)}, ${Math.round(p.peak)}°C)</option>`).join('');
  const want = S.profiles.some(p => p.id === prev) ? prev : S.selProfile;
  if (want != null) sel.value = want;
}

// Mirrors the firmware's CSV rules, for a preview before uploading.
function parseCsv(text) {
  const pts = [];
  let first = true;
  text.split(/\r?\n/).forEach((raw, n) => {
    const line = raw.trim();
    if (!line || line.startsWith('#')) return;
    const cells = line.split(/[,;\t]/).map(c => c.replace(/"/g, '').trim());
    const t = parseFloat(cells[0]), c = parseFloat(cells[1]);
    if (!isFinite(t) || !isFinite(c)) {
      if (first) { first = false; return; }
      throw new Error(`Line ${n + 1}: expected "time_s,temp_c"`);
    }
    first = false;
    if (pts.length && t <= pts[pts.length - 1][0]) throw new Error(`Line ${n + 1}: times must strictly increase`);
    pts.push([t, c]);
  });
  if (pts.length < 2) throw new Error('A profile needs at least 2 points');
  return pts;
}

function takeFile(file) {
  if (!file) return;
  const reader = new FileReader();
  reader.onload = () => {
    const drop = $('#pf-drop');
    drop.classList.remove('ready', 'bad');
    try {
      const pts = parseCsv(reader.result);
      S.upload = { csv: reader.result };
      const peak = Math.max(...pts.map(p => p[1]));
      $('#pf-drop-t').textContent = `✓ ${file.name}: ${pts.length} points, ${fmtDur(pts[pts.length - 1][0])}, peak ${Math.round(peak)}°C`;
      drop.classList.add('ready');
      if (!$('#pf-new-name').value) $('#pf-new-name').value = file.name.replace(/\.[^.]+$/, '').slice(0, 48);
    } catch (e) {
      S.upload = null;
      $('#pf-drop-t').textContent = '✕ ' + e.message;
      drop.classList.add('bad');
    }
    $('#pf-upload').disabled = !S.upload;
  };
  reader.readAsText(file);
}

/* =============================================================== settings */

const PIN_FIELDS = [
  ['ssr', 'Solid state relay', 'Heater output', 'output'],
  ['sck', 'SCK', 'Shared clock for both MAX6675s', 'output'],
  ['so', 'SO', 'Shared data line (input)', 'input'],
  ['csAmbient', 'CS · ambient', 'Chip select, ambient MAX6675', 'output'],
  ['csAux', 'CS · aux', 'Chip select, aux MAX6675', 'output'],
];

async function loadSettings() {
  const locked = S.role !== 'admin';
  $('#set-unlock').classList.toggle('hidden', !locked);
  $('#set-form').classList.toggle('hidden', locked);
  if (locked) { setTimeout(() => $('#set-admin-pw').focus(), 50); return; }
  const d = await api('/api/settings');
  S.settings = d.settings; S.defaults = d.defaults; S.rules = d.rules;
  fillSettings(S.settings);
}

function fillSettings(s) {
  $('#set-pins').innerHTML = PIN_FIELDS.map(([key, name, help, kind]) =>
    `<label for="pin-${key}">${name}<small>${help}</small></label>
     <select id="pin-${key}" data-pin="${key}">${S.rules[kind].map(p => `<option value="${p}">GPIO ${p}</option>`).join('')}</select>`).join('');
  PIN_FIELDS.forEach(([key]) => { $('#pin-' + key).value = s.pins[key]; });
  $$('#set-pins select').forEach(sel => sel.addEventListener('change', checkPinConflicts));
  $('#set-activelow').checked = s.ssrActiveLow;
  $('#set-kp').value = s.pid.kp; $('#set-ki').value = s.pid.ki; $('#set-kd').value = s.pid.kd;
  $('#set-window').value = s.ssrWindowMs; $('#set-maxtemp').value = s.maxTempC; $('#set-door').value = s.doorMarginC;
  checkPinConflicts();
}

function checkPinConflicts() {
  const sels = $$('#set-pins select'), counts = {};
  sels.forEach(s => { counts[s.value] = (counts[s.value] || 0) + 1; });
  sels.forEach(s => s.classList.toggle('bad', counts[s.value] > 1));
  return sels.every(s => counts[s.value] === 1);
}

function readSettingsForm() {
  const pins = {};
  PIN_FIELDS.forEach(([key]) => { pins[key] = +$('#pin-' + key).value; });
  return {
    pins, ssrActiveLow: $('#set-activelow').checked,
    pid: { kp: +$('#set-kp').value, ki: +$('#set-ki').value, kd: +$('#set-kd').value },
    ssrWindowMs: +$('#set-window').value, maxTempC: +$('#set-maxtemp').value, doorMarginC: +$('#set-door').value,
  };
}

/* ================================================================== admin */

function askAdmin() {
  if (S.role === 'admin') return Promise.resolve(true);
  return new Promise(resolve => {
    const modal = $('#modal'), form = $('#modal-form'), pw = $('#modal-pw');
    pw.value = ''; $('#modal-err').textContent = '';
    modal.classList.remove('hidden');
    setTimeout(() => pw.focus(), 30);
    const close = ok => { modal.classList.add('hidden'); form.onsubmit = null; $('#modal-cancel').onclick = null; resolve(ok); };
    $('#modal-cancel').onclick = () => close(false);
    form.onsubmit = async e => {
      e.preventDefault();
      try { await elevate(pw.value); close(true); } catch (err) { $('#modal-err').textContent = err.message; shake(form); }
    };
  });
}

async function elevate(password) {
  await api('/api/elevate', { method: 'POST', body: { password } });
  S.role = 'admin';
  updateRoleUi();
}

function updateRoleUi() {
  $('#settings-lock').textContent = S.role === 'admin' ? '' : '🔒';
}

/* ================================================================== about */

const Marquee = { timer: null, step: 0, pattern: 0, bulbs: [], ro: null };

function roundRectPoint(d, x, y, W, H, r) {
  const arc = (cx, cy, a) => [cx + r * Math.cos(a), cy + r * Math.sin(a)];
  const sw = W - 2 * r, sh = H - 2 * r, q = Math.PI * r / 2;
  const segs = [
    [sw, t => [x + r + t, y]],
    [q, t => arc(x + W - r, y + r, -Math.PI / 2 + t / r)],
    [sh, t => [x + W, y + r + t]],
    [q, t => arc(x + W - r, y + H - r, t / r)],
    [sw, t => [x + W - r - t, y + H]],
    [q, t => arc(x + r, y + H - r, Math.PI / 2 + t / r)],
    [sh, t => [x, y + H - r - t]],
    [q, t => arc(x + r, y + r, Math.PI + t / r)],
  ];
  for (const [len, f] of segs) { if (d <= len) return f(d); d -= len; }
  return [x + r, y];
}

function buildBulbs() {
  const box = $('#marquee'), holder = $('#bulbs');
  const w = box.clientWidth, h = box.clientHeight;
  if (!w) return;
  const inset = 18, r = 16, W = w - 2 * inset, H = h - 2 * inset;
  const per = 2 * (W - 2 * r) + 2 * (H - 2 * r) + 2 * Math.PI * r;
  const n = Math.max(16, Math.round(per / 30));
  holder.innerHTML = '';
  Marquee.bulbs = [];
  for (let i = 0; i < n; i++) {
    const [x, y] = roundRectPoint(i * per / n, inset, inset, W, H, r);
    const b = document.createElement('div');
    b.className = 'bulb';
    b.style.left = x + 'px'; b.style.top = y + 'px';
    holder.append(b);
    Marquee.bulbs.push(b);
  }
}

// Theatre chase patterns; each returns whether bulb i is lit at step s.
const CHASE = [
  (i, s) => (i + s) % 3 === 0,                  // classic three-phase chase
  (i, s, n) => ((s - i) % n + n) % n < 6,        // a comet running around
  (i, s) => (i + s) % 4 < 2,                    // pairs marching
  (i, s) => (i + Math.floor(s / 3)) % 2 === 0,  // alternate blink
];

function marqueeTick() {
  const n = Marquee.bulbs.length;
  Marquee.step++;
  if (Marquee.step % 80 === 0) Marquee.pattern = (Marquee.pattern + 1) % CHASE.length;
  const lit = CHASE[Marquee.pattern];
  Marquee.bulbs.forEach((b, i) => {
    const flicker = Math.random() < 0.01; // an old bulb hiccups now and then
    b.classList.toggle('lit', lit(i, Marquee.step, n) !== flicker);
  });
}

function startMarquee() {
  if (Marquee.timer) return;
  if (!Marquee.ro) Marquee.ro = new ResizeObserver(buildBulbs);
  Marquee.ro.observe($('#marquee'));
  buildBulbs();
  Marquee.timer = setInterval(marqueeTick, 140);
  $('#about-meta').textContent = `Firmware v${S.info.version || '?'} · Wi-Fi “${S.info.ssid || ''}” · http://192.168.4.1/`;
}

function stopMarquee() {
  clearInterval(Marquee.timer); Marquee.timer = null;
  if (Marquee.ro) Marquee.ro.disconnect();
}

/* ============================================================== nav/login */

const TABS = ['home', 'hold', 'profiles', 'reflow', 'autotune', 'settings', 'about'];
const ON_ENTER = {
  profiles: () => act(null, loadProfiles),
  reflow: () => act(null, loadProfiles),
  settings: () => act(null, loadSettings),
  about: startMarquee,
  hold: () => {
    const st = S.status;
    if (st && st.mode === 'hold') { $('#hold-sp').value = Math.round(st.setpoint); segSet('hold-sensor', st.sensor); }
  },
};

function selectTab(t) {
  if (!TABS.includes(t)) t = 'home';
  if (S.tab === 'about' && t !== 'about') stopMarquee();
  S.tab = t;
  $$('#tabs button').forEach(b => b.classList.toggle('on', b.dataset.tab === t));
  $$('.tab').forEach(s => s.classList.toggle('on', s.dataset.tab === t));
  if (location.hash !== '#' + t) history.replaceState(null, '', '#' + t);
  if (ON_ENTER[t]) ON_ENTER[t]();
  render();
}

function showLogin() {
  clearTimeout(S.pollTimer);
  S.role = 'none';
  stopMarquee();
  $('#door').classList.remove('show');
  $('#app').classList.add('hidden');
  $('#login').classList.remove('hidden');
  $('#login-pw').value = '';
  setTimeout(() => $('#login-pw').focus(), 30);
}

function showApp() {
  $('#login').classList.add('hidden');
  $('#app').classList.remove('hidden');
  updateRoleUi();
  S.lastEvent = null;
  poll().then(() => selectTab(location.hash.slice(1) || S.tab));
  act(null, loadProfiles);
}

const segVal = name => $(`.seg[data-name="${name}"] button.on`).dataset.v;
function segSet(name, v) { $$(`.seg[data-name="${name}"] button`).forEach(b => b.classList.toggle('on', b.dataset.v === v)); }

/* ================================================================ wiring */

function wire() {
  renderLegends();

  $$('.seg[data-name]').forEach(seg => seg.addEventListener('click', e => {
    const b = e.target.closest('button[data-v]');
    if (!b) return;
    e.preventDefault();
    $$('button', seg).forEach(x => x.classList.toggle('on', x === b));
  }));

  $('#tabs').addEventListener('click', e => { const b = e.target.closest('button[data-tab]'); if (b) selectTab(b.dataset.tab); });
  window.addEventListener('hashchange', () => selectTab(location.hash.slice(1)));

  $('#login-form').addEventListener('submit', async e => {
    e.preventDefault();
    $('#login-err').textContent = '';
    try {
      const d = await api('/api/login', { method: 'POST', body: { password: $('#login-pw').value } });
      S.role = d.role;
      S.info = await api('/api/session').catch(() => S.info);
      showApp();
    } catch (err) {
      $('#login-err').textContent = err.message;
      shake($('#login-form'));
    }
  });

  $('#logout').onclick = async () => { try { await api('/api/logout', { method: 'POST', body: {} }); } catch { /* ignore */ } showLogin(); };
  $('#stop-all').onclick = e => act(e.currentTarget, () => api('/api/stop', { method: 'POST', body: {} }).then(poll));

  // home
  $('#h-on').onclick = e => act(e.currentTarget, () => api('/api/heater', { method: 'POST', body: { on: true } }).then(poll));
  $('#h-off').onclick = e => act(e.currentTarget, () => api('/api/heater', { method: 'POST', body: { on: false } }).then(poll));

  // hold
  $('#hold-start').onclick = e => act(e.currentTarget, async () => {
    await api('/api/hold', { method: 'POST', body: { setpoint: +$('#hold-sp').value, sensor: segVal('hold-sensor') } });
    await poll();
  });
  $('#hold-sp').addEventListener('keydown', e => { if (e.key === 'Enter') $('#hold-start').click(); });
  $('#hold-stop').onclick = e => act(e.currentTarget, () => api('/api/stop', { method: 'POST', body: {} }).then(poll));

  // profiles
  $('#pf-list').addEventListener('click', e => {
    const li = e.target.closest('li[data-id]');
    if (!li) return;
    S.selProfile = +li.dataset.id;
    $$('#pf-list li').forEach(x => x.classList.toggle('on', x === li));
    renderProfileDetail();
  });
  $('#pf-file').addEventListener('change', e => takeFile(e.target.files[0]));
  const drop = $('#pf-drop');
  drop.addEventListener('dragover', e => { e.preventDefault(); drop.classList.add('over'); });
  drop.addEventListener('dragleave', () => drop.classList.remove('over'));
  drop.addEventListener('drop', e => { e.preventDefault(); drop.classList.remove('over'); takeFile(e.dataTransfer.files[0]); });
  $('#pf-upload').onclick = e => act(e.currentTarget, async () => {
    const name = $('#pf-new-name').value.trim();
    if (!name) { $('#pf-new-name').focus(); throw new Error('Give the profile a name'); }
    const d = await api('/api/profiles', { method: 'POST', body: { name, description: $('#pf-new-desc').value, csv: S.upload.csv } });
    toast(`Uploaded "${d.profile.name}"`);
    S.upload = null; S.selProfile = d.profile.id;
    $('#pf-new-name').value = ''; $('#pf-new-desc').value = ''; $('#pf-file').value = '';
    $('#pf-drop-t').textContent = 'Drop a CSV here or tap to choose';
    $('#pf-drop').classList.remove('ready', 'bad');
    await loadProfiles();
  });
  $('#pf-save').onclick = e => act(e.currentTarget, async () => {
    await api(`/api/profile?id=${S.selProfile}`, { method: 'PUT', body: { name: $('#pf-name').value.trim(), description: $('#pf-desc').value } });
    toast('Profile saved');
    await loadProfiles();
  });
  $('#pf-del').onclick = e => act(e.currentTarget, async () => {
    const p = S.profiles.find(p => p.id === S.selProfile);
    if (!p || !confirm(`Delete "${p.name}"? This can't be undone.`)) return;
    await api(`/api/profile?id=${p.id}`, { method: 'DELETE' });
    toast(`Deleted "${p.name}"`);
    S.selProfile = null;
    await loadProfiles();
  });
  $('#pf-use').onclick = () => { $('#rf-profile').value = S.selProfile; S.rfDismissed = true; selectTab('reflow'); };

  // reflow
  $('#rf-profile').addEventListener('change', () => { S.rfDismissed = true; render(); });
  $('#rf-start').onclick = e => act(e.currentTarget, async () => {
    await api('/api/reflow/start', { method: 'POST', body: { profileId: +$('#rf-profile').value, sensor: segVal('rf-sensor') } });
    S.rfDismissed = false;
    await poll();
  });
  $('#rf-stop').onclick = e => act(e.currentTarget, async () => {
    if (!confirm('Abort the reflow? The heater will switch off.')) return;
    await api('/api/stop', { method: 'POST', body: {} });
    await poll();
  });

  // autotune
  $('#at-start').onclick = e => act(e.currentTarget, async () => {
    await api('/api/autotune/start', { method: 'POST', body: {
      target: +$('#at-target').value, hysteresis: +$('#at-hyst').value, cycles: +$('#at-cycles').value, sensor: segVal('at-sensor'),
    } });
    await poll();
  });
  $('#at-stop').onclick = e => act(e.currentTarget, () => api('/api/stop', { method: 'POST', body: {} }).then(poll));
  $('#at-rules').addEventListener('click', e => {
    const b = e.target.closest('button[data-rule]');
    if (!b) return;
    act(b, async () => {
      if (!await askAdmin()) return;
      await api('/api/autotune/apply', { method: 'POST', body: { rule: b.dataset.rule } });
      await poll();
    });
  });

  // settings
  $('#set-unlock').addEventListener('submit', async e => {
    e.preventDefault();
    try { await elevate($('#set-admin-pw').value); $('#set-admin-pw').value = ''; await loadSettings(); toast('Settings unlocked'); } catch (err) { toast(err.message, 'error'); shake($('#set-unlock')); }
  });
  $('#set-form').addEventListener('submit', e => {
    e.preventDefault();
    if (!checkPinConflicts()) { toast('Each pin can only be used once', 'error'); return; }
    act($('#set-form button[type=submit]'), async () => {
      const next = readSettingsForm();
      const pinsChanged = JSON.stringify(next.pins) !== JSON.stringify(S.settings.pins);
      if (pinsChanged && S.status && S.status.mode !== 'idle' && !confirm('Changing pins stops the current run. Continue?')) return;
      const d = await api('/api/settings', { method: 'PUT', body: next });
      S.settings = d.settings;
      fillSettings(S.settings);
      toast(d.pinsChanged ? 'Saved. GPIO pins re-initialized.' : 'Settings saved');
    });
  });
  $('#set-revert').onclick = () => S.settings && fillSettings(S.settings);
  $('#set-defaults').onclick = () => { if (S.defaults) { fillSettings(S.defaults); toast('Defaults loaded. Press Save to apply them.', 'warn'); } };
}

async function boot() {
  wire();
  try {
    S.info = await api('/api/session');
    S.role = S.info.role;
  } catch { S.role = 'none'; }
  if (S.role === 'none') showLogin(); else showApp();
}

boot();
