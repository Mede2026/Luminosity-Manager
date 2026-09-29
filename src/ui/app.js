// Luminosity Manager - interface. Parle avec le programme C++ par messages JSON (WebView2).
'use strict';

const $ = (id) => document.getElementById(id);
const host = window.chrome && window.chrome.webview;
const send = (cmd, data = {}) => host && host.postMessage(Object.assign({ cmd }, data));

let state = {};
let hist = null;
let hotkeys = [];
let capturing = -1;          // index du raccourci en cours de saisie
let curvePts = [[0, 0], [8000, 1]];   // courbe lux -> position (envoyee par le programme)

// ---------- Navigation ----------
function showPage(name) {
  if (capturing >= 0) { capturing = -1; send('hotkeyCapture', { value: 0 }); renderKeys(); }
  document.querySelectorAll('.nav').forEach((n) => n.classList.toggle('active', n.dataset.page === name));
  document.querySelectorAll('.page').forEach((p) => p.classList.toggle('active', p.id === 'page-' + name));
  if (name === 'camera') send('listCameras');
  if (name === 'stats') send('getStats');
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
// Exposition : en unites Windows, -6 = 2^-6 s = 1/64 s
function fmtExp(v, logUnits) {
  if (!logUnits) return String(v);
  return v < 0 ? '1/' + Math.round(Math.pow(2, -v)) + ' s' : Math.round(Math.pow(2, v)) + ' s';
}
// Meme calcul que le programme : lux -> position sur la courbe (0..1)
function curveT(lux) {
  const x = Math.log10(Math.max(0, lux) + 1);
  for (let i = 1; i < curvePts.length; i++) {
    const x0 = Math.log10(curvePts[i - 1][0] + 1), x1 = Math.log10(curvePts[i][0] + 1);
    if (x <= x1) return curvePts[i - 1][1] + ((curvePts[i][1] - curvePts[i - 1][1]) * (x - x0)) / (x1 - x0);
  }
  return 1;
}
// Couleur d'une lumiere a k Kelvin (meme formule que le programme), pour les pastilles
function kelvinRgb(k) {
  const t = k / 100;
  let r, g, b;
  if (t <= 66) { r = 255; g = 99.4708025861 * Math.log(t) - 161.1195681661; }
  else { r = 329.698727446 * Math.pow(t - 60, -0.1332047592); g = 288.1221695283 * Math.pow(t - 60, -0.0755148492); }
  b = t >= 66 ? 255 : t <= 19 ? 0 : 138.5177312231 * Math.log(t - 10) - 305.0447927307;
  const c = (v) => Math.round(Math.max(0, Math.min(255, v)));
  return `rgb(${c(r)}, ${c(g)}, ${c(b)})`;
}
const kelvinName = (k) => k < 3300 ? 'très chaude' : k < 4500 ? 'chaude' : k < 5800 ? 'neutre' : k < 7000 ? 'lumière du jour' : 'froide';
const kelvinPos = (k) => Math.max(0, Math.min(100, ((k - 2500) / (8000 - 2500)) * 100)) + '%';

function learnAt(learn, t) {
  if (!learn || !learn.length) return 0;
  const pos = t * (learn.length - 1), i = Math.floor(pos);
  if (i >= learn.length - 1) return learn[learn.length - 1];
  return learn[i] + (learn[i + 1] - learn[i]) * (pos - i);
}
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
  const PAUSES = ['', 'ordi verrouillé', 'écran éteint', 'veille', 'jeu ou vidéo en plein écran', 'jeu (' + s.profileApp + ')'];
  if (!on) { pill.classList.add('off'); text = 'Désactivé : la luminosité ne change plus'; }
  else if (s.pauseReason > 0) { pill.classList.add('off'); text = 'En pause : ' + PAUSES[s.pauseReason]; }
  else if (s.profilePct >= 0) { pill.classList.add('profile'); text = 'Profil ' + s.profileApp + ' : ' + s.profilePct + ' %'; }
  else {
    pill.classList.add('live');
    text = s.ago < 0 ? 'Première mesure…' : s.ago < 2 ? 'Actif · mesuré à l\'instant' : 'Actif · mesuré il y a ' + s.ago + ' s';
    if (s.camBusy) text += ' · webcam occupée par une autre app';
  }
  $('badge-eco').hidden = !(on && s.ecoActive);
  $('badge-read').hidden = !(on && s.readMode);
  $('btn-read').classList.toggle('on', !!s.readMode);
  $('btn-read').querySelector('span').textContent = s.readMode ? 'Quitter la lecture' : 'Mode lecture';
  pill.querySelector('span').textContent = text;

  $('source').textContent = SOURCES[s.source] || '—';
  $('light').textContent = s.source === 'sun' ? 'Soleil ' + (s.sunElev > 0 ? '+' : '') + s.sunElev + '°'
    : (s.source === 'camera' ? '≈ ' : '') + fmtLux(s.lux);
  const extra = s.offset + (s.learnNow || 0);
  $('calc').textContent = s.detected < 0 ? '—' : s.detected + ' % ' + (extra ? (extra > 0 ? '+ ' : '− ') + Math.abs(extra) + ' % ' : '') + '→ ' + s.applied + ' %';
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
  $('cam-exp').textContent = cam && s.camManualOk ? fmtExp(s.camExposure, s.camLogUnits) : '—';
  $('camLocked').checked = s.camLocked;
  $('camLocked').disabled = !s.camManualOk;
  const ex = $('camExp');
  if (s.camExpMax > s.camExpMin) {
    ex.min = s.camExpMin; ex.max = s.camExpMax; ex.step = s.camExpStep || 1;
    if (document.activeElement !== ex) { setRange(ex, s.camLockValue); $('camExp-val').textContent = fmtExp(s.camLockValue, s.camLogUnits); }
  }
  $('exp-row').classList.toggle('disabled', !s.camLocked || !s.camManualOk);
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
    let [cls, txt] = modes[s.camQuality] || modes[2];
    if (s.camLocked && s.camQuality === 0) txt = 'Exposition verrouillée : toujours la même.';
    if (s.camClip === 1) { cls = 'meh'; txt = 'Image trop sombre pour bien mesurer' + (s.camLocked ? ' : pousse l\'exposition vers la droite.' : '.'); }
    if (s.camClip === 2) { cls = 'meh'; txt = 'Image trop claire (blanche) pour bien mesurer' + (s.camLocked ? ' : pousse l\'exposition vers la gauche.' : '.'); }
    q.className = 'quality ' + cls;
    q.textContent = (s.camUsed ? s.camUsed + ' · ' : '') + txt;
  }

  // Energie et jeux
  if (document.activeElement !== $('batteryMode')) $('batteryMode').value = String(s.batteryMode);
  if (document.activeElement !== $('batteryCut')) { setRange($('batteryCut'), s.batteryCut); $('batteryCut-val').textContent = '−' + s.batteryCut + ' %'; }
  $('cut-row').classList.toggle('disabled', !s.batteryMode);
  $('eco-state').textContent = s.ecoActive
    ? 'Active maintenant (' + (s.batterySaver ? 'économiseur de batterie de Windows' : 'sur batterie') + ').'
    : !s.batteryMode ? 'Désactivée.' : s.onBattery ? 'Sur batterie, mais pas active (réglée pour l\'économiseur de Windows).' : 'Pas active : le PC est branché.';
  $('pauseFullscreen').checked = s.pauseFullscreen;
  $('external').checked = s.external;

  // True Tone
  $('tt-home').textContent = !s.trueTone ? 'désactivé' : s.displayK + ' K';
  $('trueTone').checked = s.trueTone;
  if (document.activeElement !== $('ttStrength')) { setRange($('ttStrength'), s.ttStrength); $('ttStrength-val').textContent = s.ttStrength + ' %'; }
  $('tt-strength-row').classList.toggle('disabled', !s.trueTone);
  if (s.ambientK > 0) {
    $('tt-amb').textContent = s.ambientK + ' K';
    $('tt-amb-name').textContent = kelvinName(s.ambientK);
    $('tt-amb-sw').style.background = kelvinRgb(s.ambientK);
    $('tt-mark-amb').style.left = kelvinPos(s.ambientK);
  }
  const dk = s.trueTone ? s.displayK : 6500;
  $('tt-disp').textContent = dk + ' K';
  $('tt-disp-name').textContent = s.trueTone ? kelvinName(dk) : 'normal (True Tone désactivé)';
  $('tt-disp-sw').style.background = kelvinRgb(dk);
  $('tt-mark-disp').style.left = kelvinPos(dk);
  $('tt-source').textContent = ['—', 'Capteur de couleur du PC : mesure précise.',
    'Balance des blancs de la webcam : la caméra mesure la couleur de la lumière à chaque photo.',
    'Heure du jour et soleil : pas de capteur de couleur ni de webcam disponible.'][s.ttSource] || '—';
  const note = $('tt-note');
  if (s.trueTone && !s.ttOk) { note.className = 'intro small-note warn-text'; note.textContent = 'Windows a refusé de changer les couleurs de cet écran (certains pilotes ou écrans le bloquent).'; }
  else { note.className = 'intro small-note'; note.textContent = 'Si la « Veilleuse » de Windows est activée, elle s\'ajoute à True Tone. Pour le vrai effet True Tone, tu peux la désactiver.'; }

  // Ce que l'app a appris
  const learned = (s.learn || []).some((v) => Math.abs(v) >= 0.5);
  $('learn-text').textContent = learned
    ? 'Ici, à cette lumière : ' + signed(s.learnNow || 0) + '. L\'app retient tes réglages pour chaque niveau de lumière (pointillés = sans tes réglages).'
    : 'Rien pour l\'instant. Quand tu changes la luminosité toi-même, l\'app le retient pour ce niveau de lumière seulement.';
  $('btn-forget').disabled = !learned;

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
      img.data[i * 4] = bytes.charCodeAt(i * 3);
      img.data[i * 4 + 1] = bytes.charCodeAt(i * 3 + 1);
      img.data[i * 4 + 2] = bytes.charCodeAt(i * 3 + 2);
      img.data[i * 4 + 3] = 255;
    }
    ctx.putImageData(img, 0, 0);
    // Zone ignoree (ton visage, eclaire par l'ecran) : meme ovale que dans le programme
    ctx.save();
    ctx.beginPath();
    ctx.ellipse(m.w * 0.5, m.h * 0.58, m.w * 0.25, m.h * 0.40, 0, 0, Math.PI * 2);
    ctx.fillStyle = 'rgba(0, 0, 0, 0.35)';
    ctx.fill();
    ctx.setLineDash([4, 3]);
    ctx.lineWidth = 1.2;
    ctx.strokeStyle = 'rgba(255, 255, 255, 0.85)';
    ctx.stroke();
    ctx.restore();
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
// Axe horizontal = lumiere de la piece (echelle logarithmique), vertical = luminosite de l'ecran
function drawCurve() {
  const s = state, svg = $('curve');
  const box = svg.getBoundingClientRect();
  if (s.min === undefined || !box.width) return;
  const W = box.width, H = box.height, L = 36, R = 12, T = 24, B = 26;
  const pw = W - L - R, ph = H - T - B, maxLux = 8000;
  const X = (lux) => L + (Math.log10(lux + 1) / Math.log10(maxLux + 1)) * pw;
  const Y = (pct) => T + ph - (pct * ph) / 100;
  svg.innerHTML = '';
  svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
  el('rect', { x: L, y: Y(s.max), width: pw, height: Y(s.min) - Y(s.max), class: 'zone' }, svg);
  for (const v of [0, 50, 100]) {
    el('line', { x1: L, x2: W - R, y1: Y(v), y2: Y(v), class: 'grid' }, svg);
    el('text', { x: L - 8, y: Y(v) + 4, 'text-anchor': 'end', class: 'axis' }, svg).textContent = v + '%';
  }
  const marks = [[3, 'Nuit'], [30, 'Pièce sombre'], [200, 'Salon'], [1500, 'Près d\'une fenêtre'], [8000, 'Plein jour']];
  for (const [lux, name] of marks) {
    el('line', { x1: X(lux), x2: X(lux), y1: T, y2: T + ph, class: 'grid' }, svg);
    el('text', { x: X(lux), y: H - 6, 'text-anchor': lux === 8000 ? 'end' : 'middle', class: 'axis' }, svg).textContent = name;
  }
  const clamp = (v) => Math.max(s.min, Math.min(s.max, v));
  const base = [], mine = [];
  for (let i = 0; i <= 80; i++) {
    const lux = Math.pow(10, (i / 80) * Math.log10(maxLux + 1)) - 1, t = curveT(lux);
    const det = s.min + t * (s.max - s.min);
    base.push(X(lux).toFixed(1) + ',' + Y(clamp(det)).toFixed(1));
    mine.push(X(lux).toFixed(1) + ',' + Y(clamp(det + s.offset + learnAt(s.learn, t))).toFixed(1));
  }
  el('polyline', { points: base.join(' '), class: 'base' }, svg);
  el('polyline', { points: mine.join(' '), class: 'line' }, svg);
  // Points appris (seulement ceux qui ont change)
  (s.learn || []).forEach((v, i) => {
    if (Math.abs(v) < 0.5) return;
    const t = i / (s.learn.length - 1);
    let lux = 0;                                          // lux correspondant a cette position
    for (let k = 0; k <= 400; k++) { const l = Math.pow(10, (k / 400) * Math.log10(maxLux + 1)) - 1; if (curveT(l) >= t) { lux = l; break; } }
    el('circle', { cx: X(lux), cy: Y(clamp(s.min + t * (s.max - s.min) + s.offset + v)), r: 4.5, class: 'learn' }, svg);
  });
  // Tu es ici
  const here = $('here');
  if (s.lux >= 0 && s.enabled) {
    const x = X(Math.min(maxLux, s.lux)), y = Y(s.applied);
    el('line', { x1: x, x2: x, y1: y, y2: T + ph, class: 'me-line' }, svg);
    el('circle', { cx: x, cy: y, r: 6, class: 'me' }, svg);
    here.hidden = false;
    here.style.left = Math.max(60, Math.min(W - 60, x)) + 'px';
    here.style.top = y + 'px';
    here.textContent = 'Tu es ici : ' + (s.source === 'camera' ? '≈ ' : '') + fmtLux(s.lux) + ' → ' + s.applied + ' %';
  } else here.hidden = true;
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
    const game = p.pct === 0;
    row.innerHTML = `<div class="app-icon"></div><div class="name"></div><div class="bar"${game ? ' hidden' : ''}><i></i></div>
      <div class="pct${game ? ' game' : ''}">${game ? '🎮 Jeu : app désactivée' : p.pct + ' %'}</div><button class="icon-btn" title="Supprimer"><svg><use href="#i-trash"/></svg></button>`;
    row.querySelector('.app-icon').textContent = p.exe.charAt(0).toUpperCase();
    row.querySelector('.name').textContent = p.exe;
    row.querySelector('.bar i').style.width = p.pct + '%';
    row.querySelector('.name').onclick = () => {
      $('p-exe').value = p.exe;
      $('p-game').checked = game;
      $('p-pct-ctl').classList.toggle('disabled', game);
      if (!game) { setRange($('p-pct'), p.pct); $('p-pct-val').textContent = p.pct + ' %'; }
    };
    row.querySelector('button').onclick = () => send('deleteProfile', { text: p.exe });
    box.appendChild(row);
  }
}

// ---------- Raccourcis ----------
const KEY_NAMES = ['Plus clair (+5 %)', 'Plus sombre (−5 %)', 'Mesurer maintenant', 'Activer / désactiver l\'app', 'Mode lecture (plus sombre + chaud)'];
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
$('camLocked').onchange = (e) => send('setCamLocked', { value: e.target.checked ? 1 : 0 });
$('camExp').oninput = (e) => { setRange(e.target, +e.target.value); $('camExp-val').textContent = fmtExp(+e.target.value, state.camLogUnits); };
$('camExp').onchange = (e) => { send('setCamExposure', { value: +e.target.value }); toast('Nouvelle photo avec cette exposition…'); };
$('btn-forget').onclick = () => send('resetLearning');
$('trueTone').onchange = (e) => send('setTrueTone', { value: e.target.checked ? 1 : 0 });
$('ttStrength').oninput = (e) => { setRange(e.target, +e.target.value); $('ttStrength-val').textContent = e.target.value + ' %'; };
$('ttStrength').onchange = (e) => send('setTTStrength', { value: +e.target.value });
// Reperes de l'echelle True Tone places a leur vraie position
document.querySelectorAll('.tt-labels span').forEach((sp) => {
  const k = parseInt(sp.textContent, 10);
  sp.style.position = 'absolute';
  sp.style.left = kelvinPos(k);
  sp.style.transform = k <= 2500 ? 'none' : k >= 8000 ? 'translateX(-100%)' : 'translateX(-50%)';
});

$('p-game').onchange = (e) => $('p-pct-ctl').classList.toggle('disabled', e.target.checked);
$('p-pct').oninput = (e) => { setRange(e.target, +e.target.value); $('p-pct-val').textContent = e.target.value + ' %'; };
$('btn-pick').onclick = () => send('pickApp');
$('btn-add').onclick = () => {
  const exe = $('p-exe').value.trim();
  if (!exe) { toast('Écris le nom de l\'app ou clique sur « App active ».'); return; }
  send('addProfile', { exe, pct: $('p-game').checked ? 0 : +$('p-pct').value });   // 0 = jeu : l'app se desactive
  $('p-exe').value = '';
};
$('p-exe').onkeydown = (e) => e.key === 'Enter' && $('btn-add').click();
$('btn-keys-reset').onclick = () => send('resetHotkeys');
$('btn-read').onclick = () => send('setReadMode', { value: state.readMode ? 0 : 1 });
$('batteryMode').onchange = (e) => send('setBatteryMode', { value: +e.target.value });
$('batteryCut').oninput = (e) => { setRange(e.target, +e.target.value); $('batteryCut-val').textContent = '−' + e.target.value + ' %'; };
$('batteryCut').onchange = (e) => send('setBatteryCut', { value: +e.target.value });
$('pauseFullscreen').onchange = (e) => send('setPauseFullscreen', { value: e.target.checked ? 1 : 0 });
$('external').onchange = (e) => send('setExternal', { value: e.target.checked ? 1 : 0 });
$('btn-locate').onclick = () => { $('city-coords').textContent = 'Recherche de ta position…'; send('locate'); };
$('btn-export').onclick = () => send('exportData');
$('btn-import').onclick = () => send('importData');
$('btn-stats-reset').onclick = () => { if (confirm('Effacer toutes les statistiques ?')) send('resetStats'); };

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

// ---------- Statistiques ----------
let stats = null;
let statsRange = 7;
const CAT_NAMES = ['Nuit', 'Sombre', 'Intérieur', 'Lumineux', 'Plein jour'];
const CAT_HINTS = ['moins de 10 lux', '10 à 80 lux', '80 à 400 lux', '400 à 2000 lux', 'plus de 2000 lux'];
const MONTHS = ['janv.', 'févr.', 'mars', 'avr.', 'mai', 'juin', 'juil.', 'août', 'sept.', 'oct.', 'nov.', 'déc.'];
const DAYS = ['dim.', 'lun.', 'mar.', 'mer.', 'jeu.', 'ven.', 'sam.'];

function fmtMin(min) {
  min = Math.round(min);
  if (min < 60) return min + ' min';
  const h = Math.floor(min / 60), m = min % 60;
  return h + ' h' + (m ? ' ' + pad(m) : '');
}
const fmtNum = (n) => Math.round(n).toLocaleString('fr-CA');
function parseDate(iso) { const [y, m, d] = iso.split('-').map(Number); return new Date(y, m - 1, d); }
function fmtDate(iso, long) {
  const d = parseDate(iso);
  return long ? d.getDate() + ' ' + MONTHS[d.getMonth()] + ' ' + d.getFullYear() : d.getDate() + ' ' + MONTHS[d.getMonth()];
}

// Bulle d'info commune a tous les graphiques
const tip = document.createElement('div');
tip.className = 'chart-tip';
tip.hidden = true;
document.body.appendChild(tip);
function attachTip(el, html) {
  el.addEventListener('mouseenter', () => { tip.innerHTML = html; tip.hidden = false; });
  el.addEventListener('mousemove', (e) => { tip.style.left = e.clientX + 'px'; tip.style.top = e.clientY + 'px'; });
  el.addEventListener('mouseleave', () => (tip.hidden = true));
}

// Colonnes : items = [{ v: 0..1 ou null, tip }], grid = [{ at: 0..1, label }], xl = [{ i, text }]
function columns(box, items, grid, xl, color) {
  box.innerHTML = '';
  const plot = document.createElement('div');
  plot.className = 'plot';
  for (const g of grid) {
    const line = document.createElement('div');
    line.className = 'grid-line';
    line.style.bottom = g.at * 100 + '%';
    line.innerHTML = '<span></span>';
    line.firstChild.textContent = g.label;
    plot.appendChild(line);
  }
  let any = false;
  items.forEach((it) => {
    const col = document.createElement('div');
    col.className = 'col' + (it.v == null ? ' nodata' : '');
    const bar = document.createElement('i');
    if (it.v != null) { bar.style.height = Math.max(1.5, it.v * 100) + '%'; any = true; }
    if (color) bar.style.setProperty('--bar', color);
    col.appendChild(bar);
    if (it.v != null) attachTip(col, it.tip);
    plot.appendChild(col);
  });
  box.appendChild(plot);
  const xlab = document.createElement('div');
  xlab.className = 'xlabels';
  for (const x of xl) {
    const sp = document.createElement('span');
    sp.style.left = ((x.i + 0.5) / items.length) * 100 + '%';
    sp.textContent = x.text;
    xlab.appendChild(sp);
  }
  box.appendChild(xlab);
  if (!any) { const n = document.createElement('div'); n.className = 'none'; n.textContent = 'Pas encore de données pour cette période.'; box.appendChild(n); }
}

// Barres horizontales : rows = [{ label, value, text }]
function hbars(box, rows, empty) {
  box.innerHTML = '';
  const max = Math.max(1, ...rows.map((r) => r.value));
  rows = rows.filter((r) => r.value > 0);
  if (!rows.length) { box.innerHTML = '<div class="muted">' + empty + '</div>'; return; }
  for (const r of rows) {
    const row = document.createElement('div');
    row.className = 'hbar';
    row.innerHTML = '<span></span><div class="track"><i></i></div><b></b>';
    row.children[0].textContent = r.label;
    row.children[0].title = r.label;
    row.querySelector('i').style.width = (r.value / max) * 100 + '%';
    row.children[2].textContent = r.text;
    box.appendChild(row);
  }
}

function tile(label, value, note) {
  const t = document.createElement('div');
  t.className = 'tile';
  t.innerHTML = '<span></span><b></b><small></small>';
  t.children[0].textContent = label;
  t.children[1].textContent = value;
  t.children[2].textContent = note || '';
  return t;
}

function renderStats() {
  if (!stats) return;
  document.querySelectorAll('#stats-range button').forEach((b) => b.classList.toggle('on', +b.dataset.days === statsRange));
  const days = stats.days.filter((d) => d.ago < statsRange);
  const sum = (f) => days.reduce((a, d) => a + f(d), 0);
  const active = sum((d) => d.active);
  const brightAvg = active ? sum((d) => (d.bright >= 0 ? d.bright * d.active : 0)) / active : -1;
  const luxW = sum((d) => (d.lux >= 0 ? d.active : 0));
  const luxAvg = luxW ? Math.pow(10, sum((d) => (d.lux >= 0 ? Math.log10(d.lux + 1) * d.active : 0)) / luxW) - 1 : -1;
  const ttMin = sum((d) => d.trueTone);
  const ttAvg = ttMin ? sum((d) => (d.displayK > 0 ? d.displayK * d.trueTone : 0)) / ttMin : -1;
  // Energie : on compte ~6 W pour un ecran de portable a 100 % (estimation)
  const wh = sum((d) => (d.bright >= 0 ? (d.active / 60) * 6 * (100 - d.bright) / 100 : 0));
  const period = statsRange === 1 ? 'aujourd\'hui' : statsRange + ' derniers jours';

  const tiles = $('stats-tiles');
  tiles.innerHTML = '';
  tiles.append(
    tile('Temps actif', fmtMin(active), period),
    tile('Luminosité moyenne', brightAvg < 0 ? '—' : Math.round(brightAvg) + ' %', 'de l\'écran'),
    tile('Lumière moyenne', luxAvg < 0 ? '—' : '≈ ' + fmtLux(luxAvg), 'dans ta pièce'),
    tile('Écran économisé', brightAvg < 0 ? '—' : Math.round(100 - brightAvg) + ' %', brightAvg < 0 ? '' : '≈ ' + (wh >= 10 ? Math.round(wh) : wh.toFixed(1)).toString().replace('.', ',') + ' Wh (estimation)'),
    tile('Ajustements auto', fmtNum(sum((d) => d.adjusts)), 'changements de lumière'),
    tile('Réglés par toi', fmtNum(sum((d) => d.manual)), 'appris par l\'app'),
    tile('Photos webcam', fmtNum(sum((d) => d.photos)), 'jamais enregistrées'),
    tile('True Tone moyen', ttAvg < 0 ? '—' : Math.round(ttAvg) + ' K', ttAvg < 0 ? '' : kelvinName(ttAvg)),
  );

  // Colonnes par jour (au moins 7 jours pour que ca reste lisible)
  const span = Math.max(7, statsRange);
  const byAgo = new Map(stats.days.map((d) => [d.ago, d]));
  const today = parseDate(stats.today);
  const slots = [];
  for (let ago = span - 1; ago >= 0; ago--) {
    const date = new Date(today); date.setDate(today.getDate() - ago);
    slots.push({ date, d: byAgo.get(ago) });
  }
  const every = span <= 7 ? 1 : span <= 30 ? 5 : 15;
  const xl = slots.map((sl, i) => ({ i, text: span <= 7 ? DAYS[sl.date.getDay()] : sl.date.getDate() + ' ' + MONTHS[sl.date.getMonth()] }))
    .filter((x, i) => (slots.length - 1 - i) % every === 0);
  const dayTitle = (sl) => sl.date.getDate() + ' ' + MONTHS[sl.date.getMonth()];
  columns($('chart-days-bright'), slots.map((sl) => ({
    v: sl.d && sl.d.bright >= 0 && sl.d.active ? sl.d.bright / 100 : null,
    tip: sl.d ? `<b>${dayTitle(sl)}</b>Écran : ${Math.round(sl.d.bright)} % en moyenne<br>Actif : ${fmtMin(sl.d.active)}` : '',
  })), [{ at: 0.5, label: '50 %' }, { at: 1, label: '100 %' }], xl);
  const luxScale = (lux) => Math.log10(lux + 1) / Math.log10(8001);
  columns($('chart-days-lux'), slots.map((sl) => ({
    v: sl.d && sl.d.lux >= 0 && sl.d.active ? luxScale(sl.d.lux) : null,
    tip: sl.d ? `<b>${dayTitle(sl)}</b>Lumière : ≈ ${fmtLux(sl.d.lux)} en moyenne<br>de ${fmtLux(Math.max(0, sl.d.minLux))} à ${fmtLux(sl.d.maxLux)}` : '',
  })), [{ at: luxScale(10), label: '10' }, { at: luxScale(100), label: '100' }, { at: luxScale(1000), label: '1000' }], xl, 'var(--light)');

  // Journee type (depuis le debut)
  const hours = stats.hours.map((h, i) => ({
    v: h[0] >= 0 ? h[0] / 100 : null,
    tip: `<b>${i}h à ${i + 1}h</b>Écran : ${Math.round(h[0])} % en moyenne<br>Lumière : ≈ ${fmtLux(Math.max(0, h[1]))}`,
  }));
  columns($('chart-hours'), hours, [{ at: 0.5, label: '50 %' }, { at: 1, label: '100 %' }],
    [0, 3, 6, 9, 12, 15, 18, 21].map((i) => ({ i, text: i + 'h' })));

  // Categories de lumiere : barre empilee + legende
  const cats = [0, 1, 2, 3, 4].map((c) => sum((d) => d.cat[c] || 0));
  const catTotal = cats.reduce((a, b) => a + b, 0);
  const box = $('chart-cats');
  box.innerHTML = '';
  if (!catTotal) box.innerHTML = '<div class="muted">Pas encore de données pour cette période.</div>';
  else {
    const stack = document.createElement('div');
    stack.className = 'stack';
    cats.forEach((m, c) => {
      if (!m) return;
      const seg = document.createElement('i');
      seg.style.flex = String(m);
      seg.style.background = `var(--ord-${c + 1})`;
      attachTip(seg, `<b>${CAT_NAMES[c]}</b>${CAT_HINTS[c]}<br>${fmtMin(m)} · ${Math.round((m / catTotal) * 100)} %`);
      stack.appendChild(seg);
    });
    const legend = document.createElement('div');
    legend.className = 'legend-grid';
    cats.forEach((m, c) => {
      const it = document.createElement('div');
      it.className = 'legend-item';
      it.innerHTML = `<i style="background:var(--ord-${c + 1})"></i><span>${CAT_NAMES[c]} <small>${CAT_HINTS[c]}</small></span><b>${Math.round((m / catTotal) * 100)} %</b>`;
      legend.appendChild(it);
    });
    box.append(stack, legend);
  }

  hbars($('chart-time'), [
    { label: 'Actif', value: active, text: fmtMin(active) },
    { label: 'En pause', value: sum((d) => d.paused), text: fmtMin(sum((d) => d.paused)) },
    { label: 'App désactivée', value: sum((d) => d.off), text: fmtMin(sum((d) => d.off)) },
    { label: 'Mode lecture', value: sum((d) => d.reading), text: fmtMin(sum((d) => d.reading)) },
    { label: 'Économie d\'énergie', value: sum((d) => d.battery), text: fmtMin(sum((d) => d.battery)) },
  ], 'Pas encore de données.');
  const src = [0, 1, 2].map((i) => sum((d) => d.src[i] || 0));
  hbars($('chart-src'), [
    { label: 'Capteur de lumière', value: src[0], text: fmtMin(src[0]) },
    { label: 'Webcam', value: src[1], text: fmtMin(src[1]) },
    { label: 'Soleil (heure)', value: src[2], text: fmtMin(src[2]) },
  ], 'Pas encore de données.');
  hbars($('chart-apps'), stats.apps.slice().sort((a, b) => b.minutes - a.minutes)
    .map((a) => ({ label: a.exe, value: a.minutes, text: fmtMin(a.minutes) })), 'Aucun profil d\'app utilisé pour l\'instant.');

  $('stats-since').textContent = 'depuis le ' + fmtDate(stats.since, true);
  const tot = $('stats-totals');
  tot.innerHTML = '';
  [['Temps actif', fmtMin(stats.totalMinutes)], ['Photos webcam', fmtNum(stats.totalPhotos)],
   ['Ajustements auto', fmtNum(stats.totalAdjusts)], ['Réglés par toi', fmtNum(stats.totalManual)],
   ['En mode lecture', fmtMin(stats.totalReading)], ['En pause', fmtMin(stats.totalPaused)]].forEach(([k, v]) => {
    const d = document.createElement('div');
    d.innerHTML = '<span></span><b></b>';
    d.children[0].textContent = k;
    d.children[1].textContent = v;
    tot.appendChild(d);
  });
}
document.querySelectorAll('#stats-range button').forEach((b) => (b.onclick = () => { statsRange = +b.dataset.days; renderStats(); }));
setInterval(() => { if (document.querySelector('#page-stats.active')) send('getStats'); }, 60000);

// ---------- Messages du programme ----------
function onMessage(m) {
  switch (m.type) {
    case 'init':
      $('version').textContent = m.version;
      document.body.classList.toggle('mica', !!m.mica);
      if (m.curve) curvePts = m.curve;
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
    case 'stats':
      stats = m;
      renderStats();
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
  onMessage({ type: 'init', version: '0.4', mica: false, accent: '#0067c0', curve: [[0, 0], [3, 0.04], [10, 0.12], [30, 0.25], [80, 0.38], [200, 0.52], [400, 0.64], [800, 0.77], [1500, 0.88], [3000, 0.96], [8000, 1]] });
  onMessage({ type: 'hist', nowMin, light, bright });
  onMessage({ type: 'profiles', list: [{ exe: 'LumaFusion.exe', pct: 100 }, { exe: 'rekordbox.exe', pct: 70 }, { exe: 'GeometryDash.exe', pct: 0 }] });
  onMessage({ type: 'cameras', list: ['Integrated Camera', 'Integrated IR Camera'], choice: '' });
  onMessage({ type: 'city', ok: true, searched: false, name: 'Boucherville, Québec, Canada', lat: 45.59, lon: -73.44 });
  onMessage({ type: 'update', status: 'latest', version: '', current: '0.4' });
  onMessage({ type: 'hotkeys', list: [{ vk: 38, mods: 6 }, { vk: 40, mods: 6 }, { vk: 77, mods: 6 }, { vk: 65, mods: 6 }, { vk: 76, mods: 6 }] });
  const w = 160, h = 120;
  let px = '';
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const lamp = Math.exp(-((x - 118) ** 2 + (y - 30) ** 2) / 900);
    const base = 60 + 40 * (y / h);
    px += String.fromCharCode(Math.min(255, base + 170 * lamp | 0), Math.min(255, base + 150 * lamp | 0), Math.min(255, base - 10 + 110 * lamp | 0));
  }
  onMessage({ type: 'thumb', w, h, data: btoa(px) });
  let ago = 3;
  const tick = () => {
    onMessage({ type: 'state', enabled: true, bright: 72, lux: 340, source: 'camera', sunElev: 12, detected: 67, offset: 5,
      min: 10, max: 100, applied: 72, learnNow: 0, level: 0.6, learn: [8, 3, 0, 0, 0], camLocked: false, camLockValue: -6,
      camExposure: -6, camExpMin: -11, camExpMax: -2, camExpStep: 1, camLogUnits: true, camManualOk: true, camClip: 0, trueTone: true, ttStrength: 60, ambientK: 3400, displayK: 5300, ttSource: 2, ttOk: true, ago: ago++ % 30, camNext: 30 - (ago % 30), useCam: true, camInterval: 30, camQuality: 0,
      camLevel: 46, startup: true, updateCheck: true, lat: 45.59, lon: -73.44, camUsed: 'Integrated Camera', profileApp: '', profilePct: -1, pauseReason: 0, camBusy: false, onBattery: true, batterySaver: false, ecoActive: true,
      batteryMode: 1, batteryCut: 10, pauseFullscreen: true, readMode: false, external: true });
  };
  tick();
  setInterval(tick, 1000);
  // statistiques factices (40 jours)
  const sd = [], iso = (d) => d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate());
  let seed = 7;
  const rnd = () => ((seed = (seed * 16807) % 2147483647) / 2147483647);
  for (let ago = 39; ago >= 0; ago--) {
    if (ago === 12 || ago === 13) continue;               // jours sans l'ordi
    const d = new Date(); d.setDate(d.getDate() - ago);
    const active = Math.round(120 + rnd() * 360), lux = 40 + rnd() * 400;
    sd.push({ date: iso(d), ago, active, paused: Math.round(rnd() * 90), off: Math.round(rnd() * 20), reading: Math.round(rnd() * 40),
      battery: Math.round(rnd() * 150), trueTone: active, bright: 35 + rnd() * 40, lux, minLux: 2, maxLux: 1800, displayK: 5200 + rnd() * 900,
      src: [0, Math.round(active * 0.8), Math.round(active * 0.2)],
      cat: [Math.round(active * 0.12), Math.round(active * 0.25), Math.round(active * 0.4), Math.round(active * 0.18), Math.round(active * 0.05)],
      adjusts: Math.round(10 + rnd() * 40), manual: Math.round(rnd() * 4), photos: Math.round(active * 2) });
  }
  onMessage({ type: 'stats', today: iso(new Date()), since: sd[0].date, days: sd,
    hours: Array.from({ length: 24 }, (_, h) => [h < 7 ? -1 : 20 + 50 * Math.max(0, Math.sin(((h - 6) / 15) * Math.PI)), 20 + 500 * Math.max(0, Math.sin(((h - 6) / 14) * Math.PI))]),
    apps: [{ exe: 'LumaFusion.exe', minutes: 540 }, { exe: 'GeometryDash.exe', minutes: 310 }, { exe: 'rekordbox.exe', minutes: 185 }],
    totalMinutes: 12040, totalPhotos: 21400, totalAdjusts: 1260, totalManual: 38, totalReading: 640, totalPaused: 1800 });
  if (location.hash) showPage(location.hash.slice(1));
}
