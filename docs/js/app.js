// celosia nx — installer (lemasterxar5). Three optional, independent pieces -> one ZIP.
'use strict';

const RAIZ = 'switch/celosia_nx/';
const estado = { iso: null, lsfg: null, tex: null };
let trabajando = false, cancelado = false;

const $ = id => document.getElementById(id);
const tam = n => n >= 1e9 ? (n / 1e9).toFixed(2) + ' GB' : n >= 1e6 ? (n / 1e6).toFixed(1) + ' MB' : Math.max(1, Math.round(n / 1e3)) + ' KB';

function pintarEstado(clave, texto, clase) {
  const e = $('estado-' + clave);
  e.textContent = texto; e.className = 'estado' + (clase ? ' ' + clase : '');
  $('pieza-' + clave).classList.toggle('lista', !!estado[clave]);
  document.querySelector('[data-quitar="' + clave + '"]').hidden = !estado[clave];
  pintarResumen();
}
// The port itself (app/celosia-nx-nro.js): always in the ZIP when this copy of the page carries it.
const NRO = self.CELOSIA_NRO || null;
function bytesNro() {
  const s = atob(NRO.b64), b = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) b[i] = s.charCodeAt(i);
  return b;
}
function pintarResumen() {
  const r = $('resumen'); r.textContent = '';
  const piezas = [['iso', 'Game'], ['lsfg', 'LSFG'], ['tex', 'Panda Venom']];
  let total = NRO ? NRO.size : 0, alguna = !!NRO;
  if (NRO) {
    const c = document.createElement('span');
    c.className = 'chip si'; c.textContent = '✓ celosia nx ' + NRO.version + ' · ' + tam(NRO.size);
    r.appendChild(c);
  }
  for (const [k, nombre] of piezas) {
    const c = document.createElement('span');
    c.className = 'chip' + (estado[k] ? ' si' : '');
    c.textContent = (estado[k] ? '✓ ' : '— ') + nombre + (estado[k] ? ' · ' + tam(estado[k].bytes) : '');
    r.appendChild(c);
    if (estado[k]) { alguna = true; total += estado[k].bytes; }
  }
  $('crear').disabled = !alguna || trabajando || !puedeGuardar();
  if (!trabajando) $('texto-progreso').textContent = alguna ? 'Ready to pack (' + tam(total) + ').' : 'Pick at least one piece.';
}
function puedeGuardar() { return typeof window.showSaveFilePicker === 'function'; }

// --- I. ISO ---------------------------------------------------------------------------------
async function elegirIso(file) {
  estado.iso = null;
  pintarEstado('iso', 'Checking ' + file.name + '…', 'trabajando');
  try {
    const v = await isoVerify(file, paso => pintarEstado('iso', paso, 'trabajando'));
    estado.iso = { file, list: v.list, bytes: v.bytes };
    pintarEstado('iso', (v.exact ? 'Copy verified' : 'Right edition (SCUS-97472), but the executable differs from the reference') +
      ' · ' + v.list.length + ' files, ' + tam(v.bytes) + '.', v.exact ? 'ok' : 'trabajando');
  } catch (e) {
    pintarEstado('iso', e.message, 'error');
  }
}

// --- II. Lossless.dll -> LSFG shader cache -------------------------------------------------------
// The preparer (wasm/lsfgprep-bundle.js: lsfg-deko3d + SPIRV-Cross + devkitPro uam 1.1.0, the same code and
// settings as the port's lsfgprep.exe) runs in a worker made from memory (Blob), so the page works opened
// straight from disk: no server, no commands.
let trabajador = null;
function cuerpoTrabajador() {
  /* global createLsfgPrep */
  let modulo = null;
  onmessage = async (event) => {
    const { dll, wasm } = event.data;
    try {
      if (!modulo) modulo = await createLsfgPrep({ wasmBinary: wasm, print: () => {}, printErr: m => postMessage({ type: 'log', text: m }) });
      const M = modulo;
      M.FS.writeFile('/Lossless.dll', new Uint8Array(dll));
      const result = {};
      for (const [label, perf] of [['Performance', 1], ['Quality', 0]]) {
        postMessage({ type: 'step', text: 'Preparing the ' + (perf ? 'performance' : 'quality') + ' shaders…' });
        const out = '/out/' + label;
        try { M.FS.mkdirTree(out); } catch (e) { /* exists */ }
        const rc = M.ccall('lsfgprep_run', 'number', ['string', 'string', 'number'], ['/Lossless.dll', out, perf]);
        if (rc !== 0) throw new Error(M.UTF8ToString(M.ccall('lsfgprep_error', 'number', [], [])) || ('code ' + rc));
        result[label] = M.FS.readdir(out).filter(n => n !== '.' && n !== '..').map(n => ({ name: n, data: M.FS.readFile(out + '/' + n) }));
      }
      postMessage({ type: 'done', result });
    } catch (e) {
      postMessage({ type: 'error', text: String(e && e.message || e) });
    }
  };
}
function crearTrabajador() {
  const fuente = self.LSFGPREP.js + '\n;(' + cuerpoTrabajador.toString() + ')();\n';
  return new Worker(URL.createObjectURL(new Blob([fuente], { type: 'text/javascript' })));
}
function bytesWasm() {
  const s = atob(self.LSFGPREP.wasm), b = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) b[i] = s.charCodeAt(i);
  return b.buffer;
}
async function elegirDll(file) {
  estado.lsfg = null;
  pintarEstado('lsfg', 'Reading ' + file.name + '…', 'trabajando');
  const datos = await file.arrayBuffer();
  const b = new Uint8Array(datos);
  if (b.length < 1024 || b[0] !== 0x4D || b[1] !== 0x5A) { pintarEstado('lsfg', 'This is not a valid Windows DLL.', 'error'); return; }
  if (!self.LSFGPREP) {
    pintarEstado('lsfg', 'The LSFG preparer (wasm/lsfgprep-bundle.js) is missing from this copy of the page.', 'error');
    return;
  }
  if (trabajador) trabajador.terminate();
  trabajador = crearTrabajador();
  const inicio = performance.now();
  trabajador.onmessage = ev => {
    const m = ev.data;
    if (m.type === 'step') pintarEstado('lsfg', m.text, 'trabajando');
    else if (m.type === 'log') console.log('[lsfgprep]', m.text);
    else if (m.type === 'error') { pintarEstado('lsfg', 'Could not prepare: ' + m.text, 'error'); trabajador.terminate(); trabajador = null; }
    else if (m.type === 'done') {
      const archivos = [];
      for (const modo of ['Performance', 'Quality']) for (const f of m.result[modo]) archivos.push({ ruta: 'lsfg/Cache/' + modo + '/' + f.name, data: f.data });
      const bytes = archivos.reduce((s, f) => s + f.data.length, 0);
      estado.lsfg = { archivos, bytes };
      pintarEstado('lsfg', 'Shaders prepared: ' + archivos.length + ' files (' + tam(bytes) + ') in ' +
        Math.round((performance.now() - inicio) / 1000) + ' s.', 'ok');
      trabajador.terminate(); trabajador = null;
    }
  };
  const wasm = bytesWasm();
  trabajador.postMessage({ dll: datos, wasm }, [datos, wasm]);
}

// --- III. Panda Venom ---------------------------------------------------------------------------
// The game reads textures/pandavenom/ flat and only loads <TEX0 hash>-<CLUT hash>-<bits>.dds
// (game/overrides/sotc_texpack.cpp): subfolders such as "replacements/" are flattened, anything else is skipped.
// The pack (any edition's folder name: the textures are matched by hash) has replacements/ (with UI/) and
// "_Optional buttons/{Playstation,Xbox}": alternative copies of textures that are also in replacements/.
// Default: replacements/ only, as the pack ships; a chosen button style replaces those few files.
const NOMBRE_TEXTURA = /^[0-9a-f]+-[0-9a-f]+-[0-9a-f]+\.dds$/i;
let texturasElegidas = null;
function estiloDe(f) {
  const ruta = (f.webkitRelativePath || f.name).toLowerCase();
  if (!ruta.includes('_optional')) return 'main';
  if (ruta.includes('/xbox/')) return 'xbox';
  if (ruta.includes('/playstation/')) return 'playstation';
  return 'otro';
}
function elegirTexturas(lista) {
  texturasElegidas = lista;
  const estilos = new Set(lista.filter(f => NOMBRE_TEXTURA.test(f.name)).map(estiloDe));
  const sel = $('botones-tex');
  sel.hidden = !(estilos.has('xbox') || estilos.has('playstation'));
  armarTexturas();
}
function armarTexturas() {
  estado.tex = null;
  const lista = texturasElegidas || [];
  const estilo = $('botones-tex').hidden ? 'main' : $('estilo-botones').value;
  const porNombre = new Map(); let repetidas = 0, otras = 0, cambiadas = 0;
  for (const f of lista) {
    if (!NOMBRE_TEXTURA.test(f.name)) { otras++; continue; }
    if (estiloDe(f) !== 'main') continue;
    if (porNombre.has(f.name.toLowerCase())) { repetidas++; continue; }
    porNombre.set(f.name.toLowerCase(), f);
  }
  if (estilo !== 'main') for (const f of lista) {
    if (NOMBRE_TEXTURA.test(f.name) && estiloDe(f) === estilo) { porNombre.set(f.name.toLowerCase(), f); cambiadas++; }
  }
  if (!porNombre.size) { pintarEstado('tex', 'No Panda Venom textures (PCSX2 .dds names) in this folder.', 'error'); return; }
  const archivos = [...porNombre.values()];
  const bytes = archivos.reduce((s, f) => s + f.size, 0);
  estado.tex = { archivos, bytes };
  const notas = [repetidas && repetidas + ' duplicates skipped', otras && otras + ' other files skipped',
                 cambiadas && cambiadas + ' button texture' + (cambiadas > 1 ? 's' : '') + ' from the ' + (estilo === 'xbox' ? 'Xbox' : 'PlayStation') + ' set'].filter(Boolean);
  pintarEstado('tex', archivos.length + ' textures, ' + tam(bytes) + (notas.length ? ' (' + notas.join(', ') + ')' : '') + '.', 'ok');
}

// --- ZIP ----------------------------------------------------------------------------------------
async function crearZip() {
  const partes = ['iso', 'lsfg', 'tex'].filter(k => estado[k]);
  const sufijo = partes.length === 3 ? 'full' : partes.length ? partes.map(k => ({ iso: 'game', lsfg: 'lsfg', tex: 'pandavenom' })[k]).join('+') : 'app';
  let destino;
  try {
    destino = await showSaveFilePicker({ suggestedName: 'celosia_nx-' + sufijo + '.zip',
      types: [{ description: 'ZIP', accept: { 'application/zip': ['.zip'] } }] });
  } catch { return; }
  trabajando = true; cancelado = false; pintarResumen();
  $('cancelar').hidden = false;
  const total = partes.reduce((s, k) => s + estado[k].bytes, 0) + (NRO ? NRO.size : 0);
  let hecho = 0; const t0 = performance.now();
  const avanzar = n => {
    hecho += n;
    $('barra-progreso').style.width = (100 * hecho / total).toFixed(1) + '%';
    const s = (performance.now() - t0) / 1000;
    $('cifra-progreso').textContent = tam(hecho) + ' / ' + tam(total) + (s > 2 ? ' · ' + tam(hecho / s) + '/s' : '');
  };
  const fueraSiCancelado = () => { if (cancelado) throw new Error('Cancelled.'); };
  let escritura;
  try {
    escritura = await destino.createWritable();
    const zip = new ZipWriter(escritura);
    if (NRO) {
      $('texto-progreso').textContent = 'Adding celosia nx…';
      await zip.addBytes(RAIZ + 'celosia_nx.nro', bytesNro(), avanzar);
    }
    if (estado.iso) {
      $('texto-progreso').textContent = 'Copying the disc…';
      const iso = estado.iso.file;
      for (const f of estado.iso.list) {
        fueraSiCancelado();
        await zip.addFile(RAIZ + 'assets/' + f.name, iso.slice(f.offset, f.offset + f.length), avanzar, () => cancelado);
      }
    }
    if (estado.lsfg) {
      $('texto-progreso').textContent = 'Adding the LSFG shaders…';
      for (const f of estado.lsfg.archivos) { fueraSiCancelado(); await zip.addBytes(RAIZ + f.ruta, f.data, avanzar); }
    }
    if (estado.tex) {
      $('texto-progreso').textContent = 'Adding the textures…';
      for (const f of estado.tex.archivos) { fueraSiCancelado(); await zip.addFile(RAIZ + 'textures/pandavenom/' + f.name, f, avanzar, () => cancelado); }
    }
    $('texto-progreso').textContent = 'Finishing the ZIP…';
    await zip.finish();
    $('texto-progreso').textContent = 'ZIP created: ' + destino.name + '. Extract it to the root of the microSD.';
  } catch (e) {
    try { await escritura?.abort(); } catch { /* already closed */ }
    $('texto-progreso').textContent = (e.message === 'Cancelled.' ? 'Cancelled. ' : 'Error: ' + e.message + '. ') + 'The ZIP was not saved.';
    $('barra-progreso').style.width = '0';
  } finally {
    // pintarResumen() rewrites the progress line: keep the final message (created / error / cancelled) visible.
    const mensaje = $('texto-progreso').textContent;
    trabajando = false; $('cancelar').hidden = true; pintarResumen();
    $('texto-progreso').textContent = mensaje;
  }
}

// --- Music -----------------------------------------------------------------------------------------
const audio = $('audio'), botonMusica = $('musica');
let musicaQuerida = localStorage.getItem('celosia-music') !== 'off';
async function tocar() {
  try { audio.volume = 0.35; await audio.play(); botonMusica.setAttribute('aria-pressed', 'true'); botonMusica.textContent = '♪ Music: on'; }
  catch { botonMusica.setAttribute('aria-pressed', 'false'); botonMusica.textContent = '♪ Music'; }
}
function parar() { audio.pause(); botonMusica.setAttribute('aria-pressed', 'false'); botonMusica.textContent = '♪ Music: off'; }
audio.addEventListener('error', () => { botonMusica.textContent = '♪ No music'; botonMusica.title = 'Put the theme in musica/tema.mp3'; });
botonMusica.addEventListener('click', ev => {
  ev.stopPropagation();
  musicaQuerida = audio.paused;
  localStorage.setItem('celosia-music', musicaQuerida ? 'on' : 'off');
  musicaQuerida ? tocar() : parar();
});
// Browsers block sound until the first gesture: the music starts with the first click on the page.
document.addEventListener('pointerdown', () => { if (musicaQuerida && audio.paused) tocar(); }, { once: true });

// --- UI wiring ---------------------------------------------------------------------------------------
document.querySelectorAll('[data-para]').forEach(b => b.addEventListener('click', () => { if (!trabajando) $(b.dataset.para).click(); }));
$('entrada-iso').addEventListener('change', e => { const f = e.target.files[0]; e.target.value = ''; if (f) elegirIso(f); });
$('entrada-lsfg').addEventListener('change', e => { const f = e.target.files[0]; e.target.value = ''; if (f) elegirDll(f); });
$('entrada-tex').addEventListener('change', e => { const l = [...e.target.files]; e.target.value = ''; if (l.length) elegirTexturas(l); });
document.querySelectorAll('[data-quitar]').forEach(b => b.addEventListener('click', () => {
  if (trabajando) return;
  const k = b.dataset.quitar; estado[k] = null;
  if (k === 'lsfg' && trabajador) { trabajador.terminate(); trabajador = null; }
  pintarEstado(k, 'Not chosen.');
}));
$('estilo-botones').addEventListener('change', () => { if (texturasElegidas && !trabajando) armarTexturas(); });
$('crear').addEventListener('click', crearZip);
$('cancelar').addEventListener('click', () => { cancelado = true; });
window.addEventListener('beforeunload', e => { if (trabajando) { e.preventDefault(); e.returnValue = ''; } });

if (!puedeGuardar()) {
  const a = $('aviso-navegador'); a.hidden = false; a.classList.add('rojo');
  a.textContent = 'This browser cannot save large files straight to disk. Use Chrome or Edge on a computer.';
}
pintarResumen();
