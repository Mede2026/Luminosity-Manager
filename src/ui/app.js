// Luminosity Manager - interface. Parle avec le programme C++ par messages JSON (WebView2).
'use strict';

const $ = (id) => document.getElementById(id);
const host = window.chrome && window.chrome.webview;
const send = (cmd, data = {}) => host && host.postMessage(Object.assign({ cmd }, data));

let state = {};
let hist = null;
let hotkeys = [];
let capturing = -1;          // index du raccourci en cours de saisie

// ---------- Navigation ----------
function showPage(name) {
  if (capturing >= 0) { capturing = -1; send('hotkeyCapture', { value: 0 }); renderKeys(); }
  document.querySelectorAll('.nav').forEach((n) => n.classList.toggle('active', n.dataset.page === name));
  document.querySelectorAll('.page').forEach((p) => p.classList.toggle('active', p.id === 'page-' + name));
  if (name === 'camera') send('listCameras');
  if (name === 'home') drawChart();
  if (name === 'brightness') drawCurve();
}
document.querySelectorAll('.nav').forEach((n) => n.addEventListener('click', () => showPage(n.dataset.page)));

// ---------- Petits outils ----------
let toastTimer;
function toast(text) {
  const t = $('toast');
  t.textContent = text;
  t.hidden = false;
  t.style.animation = 'none';
  void t.offsetWidth;
  t.style.animation = '';
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (t.hidden = true), 4500);
}
const signed = (v) => (v > 0 ? '+' : v < 0 ? '−' : '+') + Math.abs(v) + ' %';
const fmtLux = (v) => (v >= 1000 ? (v / 1000).toFixed(1).replace('.', ',') + ' k' : Math.round(v)) + ' lux';
const fmtCoord = (v) => v.toFixed(2).replace('.', ',');
const pad = (n) => String(n).padStart(2, '0');
function setRange(el, v) {
  el.value = v;
  el.style.setProperty('--p', ((v - el.min) / (el.max - el.min)) * 100 + '%');
}

// Champs avec - et + (minimum, maximum, intervalle)
const steppers = {};
document.querySelectorAll('.stepper').forEach((s) => {
  const min = +s.dataset.min, max = +s.dataset.max;
  s.innerHTML = '<button>−</button><input type="number"><button>+</button>';
  const [minus, input, plus] = s.children;
  const commit = (v) => {
    v = Math.max(min, Math.min(max, Math.round(v) || min));
    input.value = v;
    s.dispatchEvent(new CustomEvent('change', { detail: v }));
  };
  const step = (dir) => {
    const v = +input.value;
    const big = s.dataset.unit === 's' ? (v >= 120 ? 60 : v >= 30 ? 10 : 5) : 1;
    commit(v + dir * big);
  };
  minus.onclick = () => step(-1);
  plus.onclick = () => step(1);
  input.onchange = () => commit(+input.value);
  input.onkeydown = (e) => e.key === 'Enter' && input.blur();
  steppers[s.dataset.id] = { set: (v) => document.activeElement !== input && (input.value = v), el: s };
});

// ---------- Etat (chaque seconde) ----------
const SOURCES = { sensor: 'Capteur de lumière', camera: 'Webcam', sun: 'Soleil' };

function renderState() {
  const s = state;
  const on = s.enabled;
  const b = Math.max(0, s.bright);
  $('bright').textContent = s.bright < 0 ? '--' : b;
  $('gauge-arc').style.strokeDasharray = (235.6 * b) / 100 + ' 314.2';
  document.querySelector('.gauge').classList.toggle('off', !on);

  const pill = $('status');
  pill.className = 'pill';
  let text;
  if (!on) { pill.classList.add('off'); text = 'Désactivé : la luminosité ne change plus'; }
  else if (s.profilePct >= 0) { pill.classList.add('profile'); text = 'Profil ' + s.profileApp + ' : ' + s.profilePct + ' %'; }
  else {
    pill.classList.add('live');
    text = s.ago < 0 ? 'Première mesure…' : s.ago < 2 ? 'Actif · mesuré à l\'instant' : 'Actif · mesuré il y a ' + s.ago + ' s';
  }
  pill.querySelector('span').textContent = text;

  $('source').textContent = SOURCES[s.source] || '—';
  $('light').textContent = s.source === 'sun' ? 'Soleil ' + (s.sunElev > 0 ? '+' : '') + s.sunElev + '°'
    : (s.source === 'camera' ? '≈ ' : '') + fmtLux(s.lux);
  $('calc').textContent = s.detected < 0 ? '—' : s.detected + ' % ' + (s.offset ? (s.offset > 0 ? '+ ' : '− ') + Math.abs(s.offset) + ' % ' : '') + '→ ' + s.applied + ' %';
  $('btn-toggle').textContent = on ? 'Désactiver' : 'Activer';
  $('btn-measure').disabled = !on;
  $('enabled').checked = on;
  $('master-label').textContent = on ? 'Activé' : 'Désactivé';

  // Miniature : seulement en mode webcam
  const homeThumb = $('home-thumb');
  const empty = homeThumb.querySelector('.thumb-empty span');
  empty.textContent = s.source === 'sensor' ? 'Capteur de lumière' : s.source === 'sun' ? 'Mode soleil' : 'Pas encore de photo';
  homeThumb.querySelector('.thumb-empty svg use').setAttribute('href', s.source === 'camera' ? '#i-cam' : '#i-sun');
  [homeThumb, $('cam-thumb')].forEach((t) => t.classList.toggle('has-photo', s.source === 'camera' && hasPhoto));

  // Luminosite
  if (document.activeElement !== $('offset')) { setRange($('offset'), s.offset); $('offset-val').textContent = signed(s.offset); }
  steppers.min.set(s.min);
  steppers.max.set(s.max);
  $('startup').checked = s.startup;

  // Webcam
  $('useCam').checked = s.useCam;
  steppers.interval.set(s.camInterval);
  const cam = s.source === 'camera';
  $('cam-level').textContent = cam ? s.camLevel + ' %' : '—';
  $('cam-lux').textContent = cam ? '≈ ' + fmtLux(s.lux) : '—';
  $('cam-next').textContent = !on ? 'app désactivée' : cam ? 'dans ' + s.camNext + ' s' : '—';
  const q = $('cam-quality');
  if (!s.useCam) { q.className = 'quality'; q.textContent = 'Webcam désactivée : la luminosité suit le soleil.'; }
  else if (s.source === 'sensor') { q.className = 'quality good'; q.textContent = 'Ton PC a un capteur de lumière : la webcam n\'est pas utilisée.'; }
  else if (!cam) { q.className = 'quality meh'; q.textContent = 'Aucune webcam trouvée : la luminosité suit le soleil.'; }
  else {
    const modes = [
      ['good', 'Exposition fixée par l\'app : mesure précise.'],
      ['meh', 'La caméra règle son exposition seule, mais l\'app la lit : mesure correcte.'],
      ['meh', 'La caméra ne donne pas son exposition : mesure approximative. Calibre-la.'],
    ];
    const [cls, txt] = modes[s.camQuality] || modes[2];
    q.className = 'quality ' + cls;
    q.textContent = (s.camUsed ? s.camUsed + ' · ' : '') + txt;
  }

  // Mises a jour
  $('updateCheck').checked = s.updateCheck;
  if (document.querySelector('#page-brightness.active')) drawCurve();
}

// ---------- Miniature webcam ----------
let hasPhoto = false;
function renderThumb(m) {
  const bytes = atob(m.data);
  document.querySelectorAll('.thumb canvas').forEach((c) => {
    c.width = m.w; c.height = m.h;
    const ctx = c.getContext('2d');
    const img = ctx.createImageData(m.w, m.h);
    for (let i = 0; i < m.w * m.h; i++) {
      const v = bytes.charCodeAt(i);
      img.data[i * 4] = img.data[i * 4 + 1] = img.data[i * 4 + 2] = v;
      img.data[i * 4 + 3] = 255;
    }
    ctx.putImageData(img, 0, 0);
  });
  hasPhoto = true;
}

// ---------- Graphique 24 h ----------
const NS = 'http://www.w3.org/2000/svg';
function el(tag, attrs, parent) {
  const e = document.createElementNS(NS, tag);
  for (const k in attrs) e.setAttribute(k, attrs[k]);
  if (parent) parent.appendChild(e);
  return e;
}

// Moyenne par tranches de 5 minutes : courbe plus douce
function bucket(arr, size = 5) {
  const out = [];
  for (let i = 0; i < arr.length; i += size) {
    let sum = 0, n = 0;
    for (let k = i; k < i + size && k < arr.length; k++) if (arr[k] >= 0) { sum += arr[k]; n++; }
    out.push(n ? sum / n : -1);
  }
  return out;
}

// Chemin lisse (courbe de Catmull-Rom) coupe la ou il manque des donnees
function smoothPath(pts) {
  let d = '';
  for (let s = 0; s < pts.length; ) {
    while (s < pts.length && !pts[s]) s++;
    let e = s;
    while (e < pts.length && pts[e]) e++;
    const seg = pts.slice(s, e);
    if (seg.length === 1) d += `M${seg[0][0]},${seg[0][1]}h0.1`;
    for (let i = 0; i < seg.length - 1; i++) {
      const p0 = seg[i - 1] || seg[i], p1 = seg[i], p2 = seg[i + 1], p3 = seg[i + 2] || p2;
      if (i === 0) d += `M${p1[0]},${p1[1]}`;
      const c1 = [p1[0] + (p2[0] - p0[0]) / 6, p1[1] + (p2[1] - p0[1]) / 6];
      const c2 = [p2[0] - (p3[0] - p1[0]) / 6, p2[1] - (p3[1] - p1[1]) / 6];
      d += `C${c1[0].toFixed(1)},${c1[1].toFixed(1)} ${c2[0].toFixed(1)},${c2[1].toFixed(1)} ${p2[0].toFixed(1)},${p2[1].toFixed(1)}`;
    }
    s = e;
  }
  return d;
}

let chartGeom = null;
function drawChart() {
  const svg = $('chart-svg');
  const box = $('chart').getBoundingClientRect();
  if (!box.width || !hist) return;
  const W = box.width, H = box.height, L = 36, R = 8, T = 8, B = 24;
  const pw = W - L - R, ph = H - T - B;
  svg.innerHTML = '';
  svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
  const defs = el('defs', {}, svg);
  const g = el('linearGradient', { id: 'screen-fill', x1: 0, y1: 0, x2: 0, y2: 1 }, defs);
  el('stop', { offset: '0%', 'stop-color': getComputedStyle(document.body).getPropertyValue('--accent'), 'stop-opacity': 0.22 }, g);
  el('stop', { offset: '100%', 'stop-color': getComputedStyle(document.body).getPropertyValue('--accent'), 'stop-opacity': 0 }, g);

  for (const v of [0, 50, 100]) {
    const y = T + ph - (v * ph) / 100;
    el('line', { x1: L, x2: W - R, y1: y, y2: y, class: 'grid' }, svg);
    el('text', { x: L - 8, y: y + 4, 'text-anchor': 'end', class: 'axis' }, svg).textContent = v + '%';
  }
  // Heures : toutes les 3 h
  const startMin = hist.nowMin - 1439;
  for (let i = 0; i < 1440; i++) {
    const m = (((startMin + i) % 1440) + 1440) % 1440;
    if (m % 180 !== 0) continue;
    const x = L + (i * pw) / 1439;
    el('line', { x1: x, x2: x, y1: T, y2: T + ph, class: 'grid' }, svg);
    el('text', { x, y: H - 6, 'text-anchor': 'middle', class: 'axis' }, svg).textContent = m / 60 + 'h';
  }

  const light = bucket(hist.light), screen = bucket(hist.bright);
  const n = light.length;
  const toPts = (arr) => arr.map((v, i) => (v >= 0 ? [L + (i * pw) / (n - 1), T + ph - (v * ph) / 100] : null));
  const pl = toPts(light), ps = toPts(screen);
  const any = pl.some(Boolean) || ps.some(Boolean);
  $('chart-empty').hidden = any;

  // Zone coloree sous la courbe de l'ecran
  const sPath = smoothPath(ps);
  if (sPath) {
    const first = ps.find(Boolean), last = [...ps].reverse().find(Boolean);
    if (first && last && ps.every((p, i) => p || i < ps.indexOf(first) || i > ps.lastIndexOf(last)))
      el('path', { d: sPath + `L${last[0]},${T + ph}L${first[0]},${T + ph}Z`, class: 'area' }, svg);
  }
  el('path', { d: smoothPath(pl), class: 'l-light' }, svg);
  el('path', { d: sPath, class: 'l-screen' }, svg);

  const cursor = el('line', { y1: T, y2: T + ph, class: 'cursor', visibility: 'hidden' }, svg);
  const dl = el('circle', { r: 4.5, class: 'dot-light', visibility: 'hidden' }, svg);
  const ds = el('circle', { r: 4.5, class: 'dot-screen', visibility: 'hidden' }, svg);
  chartGeom = { L, pw, T, ph, n, light, screen, startMin, cursor, dl, ds, W };
}

$('chart').addEventListener('mousemove', (e) => {
  const c = chartGeom;
  if (!c) return;
  const x = e.clientX - $('chart').getBoundingClientRect().left;
  const i = Math.round(((x - c.L) / c.pw) * (c.n - 1));
  const tip = $('tooltip');
  if (i < 0 || i >= c.n || (c.light[i] < 0 && c.screen[i] < 0)) { tip.hidden = true; hideCursor(); return; }
  const cx = c.L + (i * c.pw) / (c.n - 1);
  c.cursor.setAttribute('x1', cx); c.cursor.setAttribute('x2', cx); c.cursor.setAttribute('visibility', 'visible');
  const place = (dot, v) => {
    dot.setAttribute('visibility', v >= 0 ? 'visible' : 'hidden');
    if (v >= 0) { dot.setAttribute('cx', cx); dot.setAttribute('cy', c.T + c.ph - (v * c.ph) / 100); }
  };
  place(c.dl, c.light[i]);
  place(c.ds, c.screen[i]);
  const m = (((c.startMin + i * 5) % 1440) + 1440) % 1440;
  tip.innerHTML = `<b>${Math.floor(m / 60)}h${pad(m % 60)}</b>` +
    (c.light[i] >= 0 ? `<div><i style="background:var(--light)"></i>Lumière ${Math.round(c.light[i])} %</div>` : '') +
    (c.screen[i] >= 0 ? `<div><i style="background:var(--screen)"></i>Écran ${Math.round(c.screen[i])} %</div>` : '');
  tip.hidden = false;
  tip.style.left = Math.max(70, Math.min(c.W - 70, cx)) + 'px';
});
function hideCursor() {
  if (!chartGeom) return;
  ['cursor', 'dl', 'ds'].forEach((k) => chartGeom[k].setAttribute('visibility', 'hidden'));
}
$('chart').addEventListener('mouseleave', () => { $('tooltip').hidden = true; hideCursor(); });
window.addEventListener('resize', () => { drawChart(); drawCurve(); });

// ---------- Courbe (page Luminosite) ----------
function drawCurve() {
  const s = state, svg = $('curve');
  if (s.min === undefined) return;
  const W = 600, H = 180;
  svg.innerHTML = '';
  for (const v of [0, 50, 100]) el('line', { x1: 0, x2: W, y1: H - (v * H) / 100, y2: H - (v * H) / 100, class: 'grid' }, svg);
  el('rect', { x: 0, y: H - (s.max * H) / 100, width: W, height: ((s.max - s.min) * H) / 100, class: 'zone' }, svg);
  const pts = [], base = [];
  for (let i = 0; i <= 60; i++) {
    const t = i / 60;                                   // niveau de lumiere 0..1
    const det = s.min + t * (s.max - s.min);
    const out = Math.max(s.min, Math.min(s.max, det + s.offset));
    pts.push(`${(t * W).toFixed(1)},${(H - (out * H) / 100).toFixed(1)}`);
    base.push(`${(t * W).toFixed(1)},${(H - (det * H) / 100).toFixed(1)}`);
  }
  if (s.offset) el('polyline', { points: base.join(' '), class: 'base' }, svg);
  el('polyline', { points: pts.join(' '), class: 'line' }, svg);
}

// ---------- Profils ----------
function renderProfiles(list) {
  const box = $('profile-list');
  box.innerHTML = '';
  if (!list.length) {
    box.innerHTML = '<div class="empty">Aucun profil pour l\'instant. Ajoute une app ci-dessus.</div>';
    return;
  }
  for (const p of list) {
    const row = document.createElement('div');
    row.className = 'profile';
    row.innerHTML = `<div class="app-icon"></div><div class="name"></div><div class="bar"><i></i></div>
      <div class="pct">${p.pct} %</div><button class="icon-btn" title="Supprimer"><svg><use href="#i-trash"/></svg></button>`;
    row.querySelector('.app-icon').textContent = p.exe.charAt(0).toUpperCase();
    row.querySelector('.name').textContent = p.exe;
    row.querySelector('.bar i').style.width = p.pct + '%';
    row.querySelector('.name').onclick = () => { $('p-exe').value = p.exe; setRange($('p-pct'), p.pct); $('p-pct-val').textContent = p.pct + ' %'; };
    row.querySelector('button').onclick = () => send('deleteProfile', { text: p.exe });
    box.appendChild(row);
  }
}

// ---------- Raccourcis ----------
const KEY_NAMES = ['Plus clair (+5 %)', 'Plus sombre (−5 %)', 'Mesurer maintenant', 'Activer / désactiver l\'app'];
function keyLabel(vk) {
  const special = { 37: '←', 38: '↑', 39: '→', 40: '↓', 32: 'Espace', 13: 'Entrée', 33: 'Page ↑', 34: 'Page ↓',
    36: 'Début', 35: 'Fin', 45: 'Inser', 46: 'Suppr', 107: 'Num +', 109: 'Num −', 187: '=', 189: '-', 188: ',', 190: '.' };
  if (special[vk]) return special[vk];
  if (vk >= 112 && vk <= 135) return 'F' + (vk - 111);
  if (vk >= 96 && vk <= 105) return 'Num ' + (vk - 96);
  if ((vk >= 48 && vk <= 57) || (vk >= 65 && vk <= 90)) return String.fromCharCode(vk);
  return vk ? 'Touche ' + vk : '';
}
function renderKeys() {
  const box = $('key-list');
  box.innerHTML = '';
  hotkeys.forEach((k, i) => {
    const row = document.createElement('div');
    row.className = 'row';
    const parts = [];
    if (k.mods & 2) parts.push('Ctrl');
    if (k.mods & 4) parts.push('Alt');
    if (k.mods & 1) parts.push('Maj');
    if (k.vk) parts.push(keyLabel(k.vk));
    row.innerHTML = `<div class="row-text"><b></b>${k.failed ? '<small class="failed">Déjà utilisé par une autre app</small>' : ''}</div>
      <div class="row-ctl"><button class="key-btn"><span class="keycap"></span></button></div>`;
    row.querySelector('b').textContent = KEY_NAMES[i];
    const cap = row.querySelector('.keycap');
    const btn = row.querySelector('.key-btn');
    if (capturing === i) { btn.classList.add('capturing'); cap.textContent = 'Appuie sur les touches… (Échap = annuler)'; }
    else if (!parts.length) cap.textContent = 'Aucun';
    else parts.forEach((p) => { const kb = document.createElement('kbd'); kb.textContent = p; cap.appendChild(kb); });
    btn.onclick = () => { capturing = i; send('hotkeyCapture', { value: 1 }); renderKeys(); };
    box.appendChild(row);
  });
}
document.addEventListener('keydown', (e) => {
  if (capturing < 0) return;
  e.preventDefault();
  if (e.key === 'Escape') { capturing = -1; send('hotkeyCapture', { value: 0 }); renderKeys(); return; }
  if (['Control', 'Alt', 'Shift', 'Meta'].includes(e.key)) return;
  const mods = (e.shiftKey ? 1 : 0) | (e.ctrlKey ? 2 : 0) | (e.altKey ? 4 : 0);
  if (!(mods & 6)) { toast('Utilise au moins Ctrl ou Alt (ex. Ctrl + Alt + L).'); return; }
  send('setHotkey', { index: capturing, vk: e.keyCode, mods });
  capturing = -1;
});

// ---------- Actions de l'utilisateur ----------
$('enabled').onchange = (e) => send('setEnabled', { value: e.target.checked ? 1 : 0 });
$('btn-toggle').onclick = () => send('setEnabled', { value: state.enabled ? 0 : 1 });
$('btn-measure').onclick = () => send('measure');
$('offset').oninput = (e) => { setRange(e.target, +e.target.value); $('offset-val').textContent = signed(+e.target.value); };
$('offset').onchange = (e) => send('setOffset', { value: +e.target.value });
steppers.min.el.addEventListener('change', (e) => {
  if (e.detail >= state.max) { toast('Le minimum doit rester plus petit que le maximum.'); steppers.min.set(state.min); return; }
  send('setMinMax', { min: e.detail, max: state.max });
});
steppers.max.el.addEventListener('change', (e) => {
  if (e.detail <= state.min) { toast('Le maximum doit rester plus grand que le minimum.'); steppers.max.set(state.max); return; }
  send('setMinMax', { min: state.min, max: e.detail });
});
$('startup').onchange = (e) => send('setStartup', { value: e.target.checked ? 1 : 0 });
$('useCam').onchange = (e) => send('setUseCam', { value: e.target.checked ? 1 : 0 });
steppers.interval.el.addEventListener('change', (e) => send('setInterval', { value: e.detail }));
$('camera').onchange = (e) => { send('setCamera', { text: e.target.value }); toast('Caméra changée : nouvelle photo en cours…'); };
$('btn-calibrate').onclick = () => send('calibrate');

$('p-pct').oninput = (e) => { setRange(e.target, +e.target.value); $('p-pct-val').textContent = e.target.value + ' %'; };
$('btn-pick').onclick = () => send('pickApp');
$('btn-add').onclick = () => {
  const exe = $('p-exe').value.trim();
  if (!exe) { toast('Écris le nom de l\'app ou clique sur « App active ».'); return; }
  send('addProfile', { exe, pct: +$('p-pct').value });
  $('p-exe').value = '';
};
$('p-exe').onkeydown = (e) => e.key === 'Enter' && $('btn-add').click();
$('btn-keys-reset').onclick = () => send('resetHotkeys');

$('btn-city').onclick = () => {
  const text = $('city').value.trim();
  if (!text) return;
  $('city-coords').textContent = 'Recherche en cours…';
  send('searchCity', { text });
};
$('city').onkeydown = (e) => e.key === 'Enter' && $('btn-city').click();
$('btn-latlon').onclick = () => {
  const lat = parseFloat($('lat').value.replace(',', '.')), lon = parseFloat($('lon').value.replace(',', '.'));
  if (isNaN(lat) || isNaN(lon) || Math.abs(lat) > 90 || Math.abs(lon) > 180) {
    toast('Latitude entre −90 et 90, longitude entre −180 et 180 (ex. 45,59 et −73,44).');
    return;
  }
  send('setLatLon', { lat, lon });
};

$('updateCheck').onchange = (e) => send('setUpdateCheck', { value: e.target.checked ? 1 : 0 });
$('btn-check').onclick = () => send('checkUpdate');
$('btn-update').onclick = () => send('doUpdate');
$('btn-repo').onclick = () => send('openRepo');
document.addEventListener('contextmenu', (e) => e.preventDefault());
setRange($('p-pct'), 100);

// ---------- Messages du programme ----------
function onMessage(m) {
  switch (m.type) {
    case 'init':
      $('version').textContent = m.version;
      document.body.classList.toggle('mica', !!m.mica);
      if (m.accent && !matchMedia('(prefers-color-scheme: dark)').matches) document.documentElement.style.setProperty('--accent', m.accent);
      else document.documentElement.style.removeProperty('--accent');
      break;
    case 'state':
      state = m;
      renderState();
      break;
    case 'hist':
      hist = m;
      if (document.querySelector('#page-home.active')) drawChart();
      break;
    case 'thumb':
      renderThumb(m);
      renderState();
      break;
    case 'profiles':
      renderProfiles(m.list);
      break;
    case 'pickedApp':
      if (m.app) $('p-exe').value = m.app;
      else toast('Ouvre d\'abord l\'application voulue, puis reviens ici.');
      break;
    case 'cameras': {
      const sel = $('camera');
      sel.innerHTML = '';
      sel.add(new Option('Automatique', ''));
      m.list.forEach((c) => sel.add(new Option(c, c)));
      sel.value = m.choice;
      break;
    }
    case 'city':
      $('city-name').textContent = m.name;
      $('city-coords').textContent = m.searched && !m.ok ? 'Ville introuvable (ou pas d\'internet).'
        : 'Latitude ' + fmtCoord(m.lat) + ' · Longitude ' + fmtCoord(m.lon);
      $('lat').value = fmtCoord(m.lat);
      $('lon').value = fmtCoord(m.lon);
      if (m.searched && m.ok) toast('Position : ' + m.name);
      break;
    case 'update': {
      const st = {
        '': 'Version ' + m.current,
        checking: 'Vérification…',
        latest: 'Tu as la dernière version (' + m.current + ').',
        available: 'La version ' + m.version + ' est disponible !',
        error: 'Impossible de vérifier (pas d\'internet ?).',
        downloading: 'Téléchargement de ' + m.version + '… L\'app va redémarrer.',
        failed: 'Échec de la mise à jour. Réessaie plus tard.',
      };
      $('upd-title').textContent = 'Luminosity Manager ' + m.current;
      $('upd-status').textContent = st[m.status] || '';
      $('btn-update').hidden = !(m.status === 'available' || m.status === 'failed');
      $('btn-update').textContent = 'Mettre à jour vers ' + m.version;
      $('btn-check').disabled = m.status === 'checking' || m.status === 'downloading';
      $('upd-dot').hidden = m.status !== 'available';
      break;
    }
    case 'hotkeys':
      hotkeys = m.list;
      renderKeys();
      break;
    case 'notice':
      toast(m.text);
      break;
    case 'nav':
      showPage(m.page);
      break;
  }
}

if (host) {
  host.addEventListener('message', (e) => onMessage(e.data));
  send('ready');
} else {
  demo();   // ouvert dans un navigateur normal : fausses donnees pour voir le design
}

// ---------- Mode demo (hors de l'app) ----------
function demo() {
  const now = new Date(), nowMin = now.getHours() * 60 + now.getMinutes();
  const light = [], bright = [];
  for (let i = 0; i < 1440; i++) {
    const m = (nowMin - 1439 + i + 1440) % 1440, h = m / 60;
    const day = Math.max(0, Math.sin(((h - 6) / 14) * Math.PI));
    const l = i < 200 ? -1 : Math.round(8 + 80 * day + 6 * Math.sin(i / 17));
    light.push(l);
    bright.push(l < 0 ? -1 : Math.round(Math.min(100, 15 + l * 0.85)));
  }
  onMessage({ type: 'init', version: '0.3', mica: false, accent: '#0067c0' });
  onMessage({ type: 'hist', nowMin, light, bright });
  onMessage({ type: 'profiles', list: [{ exe: 'LumaFusion.exe', pct: 100 }, { exe: 'rekordbox.exe', pct: 70 }] });
  onMessage({ type: 'cameras', list: ['Integrated Camera', 'Integrated IR Camera'], choice: '' });
  onMessage({ type: 'city', ok: true, searched: false, name: 'Boucherville, Québec, Canada', lat: 45.59, lon: -73.44 });
  onMessage({ type: 'update', status: 'available', version: 'v0.4', current: '0.3' });
  onMessage({ type: 'hotkeys', list: [{ vk: 38, mods: 6 }, { vk: 40, mods: 6 }, { vk: 77, mods: 6 }, { vk: 65, mods: 6, failed: false }] });
  const w = 64, h = 48;
  let px = '';
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const v = 70 + 110 * Math.exp(-((x - 44) ** 2 + (y - 14) ** 2) / 300) + 30 * (y / h);
    px += String.fromCharCode(Math.min(255, v | 0));
  }
  onMessage({ type: 'thumb', w, h, data: btoa(px) });
  let ago = 3;
  const tick = () => {
    onMessage({ type: 'state', enabled: true, bright: 72, lux: 340, source: 'camera', sunElev: 12, detected: 67, offset: 5,
      min: 10, max: 100, applied: 72, ago: ago++ % 30, camNext: 30 - (ago % 30), useCam: true, camInterval: 30, camQuality: 0,
      camLevel: 46, startup: true, updateCheck: true, lat: 45.59, lon: -73.44, camUsed: 'Integrated Camera', profileApp: '', profilePct: -1 });
  };
  tick();
  setInterval(tick, 1000);
  if (location.hash) showPage(location.hash.slice(1));
}
