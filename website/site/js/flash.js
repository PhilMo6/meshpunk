// The flasher page: reads firmware/index.json, lets the user pick a device, a
// build and an install type, and runs flasher.js against a Web Serial port.

import { ESPLoader, Transport } from '../vendor/esptool-js-0.6.1/bundle.js';
import { STAGES, downloadImage, flashDevice } from './flasher.js';
import { classifyImage, findBoardTag } from './image.js';

const SITE_ROOT = new URL('../', import.meta.url);

// 115200 is esptool-js's ROM baud rate, so no baud change is requested.
const BAUD = 115200;

// Larger than any flash the supported boards carry; a bigger file is not read.
const MAX_FILE_BYTES = 64 * 1048576;

const CHANNELS = {
  stable: { title: 'Stable', note: 'Recommended.' },
  dev: { title: 'Dev', note: 'Work in progress: may be broken, unfinished, or refuse to boot.' },
};

const MODES = {
  fresh: {
    title: 'Fresh install',
    note: 'Replaces everything on the device. Settings, messages, contacts and files are erased. For a first install, or to start over.',
  },
  update: {
    title: 'Update',
    note: 'Replaces the firmware only. Settings, messages and files are kept. For a device that already runs MeshPunk installed over USB.',
  },
};

const STAGE_LABELS = {
  download: 'Download',
  connect: 'Connect',
  check: 'Check the device',
  write: 'Write',
  verify: 'Verify',
  restart: 'Restart',
};

const $ = (id) => document.getElementById(id);

const ui = {
  gate: $('gate'),
  loadError: $('load-error'),
  form: $('flasher'),
  selection: $('selection'),
  boards: $('boards'),
  builds: $('builds'),
  filePanel: $('file-panel'),
  file: $('file'),
  fileInfo: $('file-info'),
  notes: $('notes'),
  notesTag: $('notes-tag'),
  notesText: $('notes-text'),
  notesLink: $('notes-link'),
  modes: $('modes'),
  fileMode: $('file-mode'),
  summary: $('summary'),
  flashBtn: $('flash-btn'),
  allPorts: $('all-ports'),
  run: $('run'),
  stages: $('stages'),
  bar: $('bar'),
  result: $('result'),
  logBox: document.querySelector('#run .log'),
  log: $('log'),
  copyLog: $('copy-log'),
  more: $('more'),
  launcherFiles: $('launcher-files'),
  downloads: $('downloads'),
};

const state = {
  index: null,
  board: null, // slug
  build: null, // 'stable' | 'dev' | 'file'
  mode: null, // 'fresh' | 'update'
  file: null, // { name, bytes, kind, reason, tag } | { name, tooLarge: true }
  busy: false,
};

const canFlash = 'serial' in navigator;
const choices = { board: {}, build: {}, mode: {} };
let logText = '';

function h(tag, props, ...children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(props || {})) {
    if (key === 'class') node.className = value;
    else if (key === 'text') node.textContent = value;
    else node.setAttribute(key, value);
  }
  node.append(...children);
  return node;
}

function megabytes(bytes) {
  return `${(bytes / 1048576).toFixed(1)} MB`;
}

function shortDate(iso) {
  const date = new Date(iso);
  if (Number.isNaN(date.getTime())) return '';
  return date.toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' });
}

// ---- index accessors --------------------------------------------------------

const release = (channel) => state.index.channels[channel]?.release ?? null;
const boardBuild = (channel, slug) => release(channel)?.boards[slug] ?? null;
const boardName = (slug) => state.index.boards[slug]?.name ?? slug;

function layoutOf(entry) {
  return { tableSha256: entry.table_sha256, appOffset: entry.app_offset, appSize: entry.app_size };
}

// Every partition layout a published release uses for this board.
function knownLayouts(slug) {
  const layouts = [];
  for (const channel of Object.keys(CHANNELS)) {
    const entry = boardBuild(channel, slug);
    if (entry && !layouts.some((l) => l.tableSha256 === entry.table_sha256)) layouts.push(layoutOf(entry));
  }
  return layouts;
}

// ---- what the current selection means --------------------------------------

function fileVerdict() {
  const file = state.file;
  if (!file) return { ok: false, text: 'Choose a MeshPunk -merged.bin or -firmware.bin.' };
  if (file.tooLarge) {
    return { ok: false, bad: true, text: `${file.name} is larger than ${megabytes(MAX_FILE_BYTES)}; it is not a flash image for these devices.` };
  }
  if (file.kind === 'unknown') {
    return { ok: false, bad: true, text: `${file.name} cannot be flashed: ${file.reason}.` };
  }
  if (file.tag && file.tag !== state.board) {
    return {
      ok: false,
      bad: true,
      text: `${file.name} is MeshPunk for the ${boardName(file.tag)}, not for the ${boardName(state.board)}. Pick that device in step 1, or another file.`,
    };
  }
  if (file.kind === 'app' && knownLayouts(state.board).length === 0) {
    return { ok: false, bad: true, text: 'An app image needs a published release for this device to check the flash layout against, and there is none.' };
  }
  const what = file.kind === 'full' ? 'full flash image' : 'app image';
  const whose = file.tag
    ? `MeshPunk for the ${boardName(file.tag)}`
    : 'no MeshPunk board tag (a build older than v0.4.1, or not MeshPunk)';
  return { ok: true, text: `${file.name} · ${megabytes(file.bytes.length)} · ${what} · ${whose}` };
}

/** The flash job the current selection describes, or null while it is incomplete. */
function currentJob() {
  if (!state.board || !state.build) return null;
  const board = state.index.boards[state.board];
  const base = { chip: board.chip, boardName: board.name };

  if (state.build === 'file') {
    if (!fileVerdict().ok) return null;
    const full = state.file.kind === 'full';
    return {
      ...base,
      kind: state.file.kind,
      bytes: state.file.bytes,
      layouts: full ? undefined : knownLayouts(state.board),
      erases: full,
      what: state.file.name,
      button: full ? 'Erase and install this file' : 'Write this app image',
    };
  }

  const rel = release(state.build);
  const entry = rel?.boards[state.board];
  if (!entry || !state.mode) return null;
  const fresh = state.mode === 'fresh';
  return {
    ...base,
    kind: fresh ? 'full' : 'app',
    file: fresh ? entry.merged : entry.firmware,
    layouts: fresh ? undefined : [layoutOf(entry)],
    erases: fresh,
    what: `MeshPunk ${rel.tag} (${state.build})`,
    button: fresh ? `Erase and install ${rel.tag}` : `Update to ${rel.tag}`,
  };
}

// ---- rendering ----------------------------------------------------------------

function makeChoice(group, value, onPick) {
  const input = h('input', { type: 'radio', name: group, value });
  const title = h('span', { class: 'choice-title' });
  const meta = h('span', { class: 'choice-meta' });
  const label = h('label', { class: 'choice' }, input, h('span', { class: 'choice-body' }, title, meta));
  input.addEventListener('change', () => {
    if (input.checked) onPick(value);
  });
  return { label, input, title, meta };
}

function buildChoices() {
  for (const slug of Object.keys(state.index.boards)) {
    const choice = makeChoice('board', slug, pickBoard);
    choice.title.textContent = boardName(slug);
    const where = Object.keys(CHANNELS)
      .filter((channel) => boardBuild(channel, slug))
      .map((channel) => `${CHANNELS[channel].title} ${release(channel).tag}`);
    choice.meta.textContent = where.length ? where.join(' · ') : 'No published build';
    choice.input.disabled = where.length === 0;
    choices.board[slug] = choice;
    ui.boards.append(choice.label);
  }
  for (const id of [...Object.keys(CHANNELS), 'file']) {
    const choice = makeChoice('build', id, pickBuild);
    choices.build[id] = choice;
    ui.builds.append(choice.label);
  }
  for (const id of Object.keys(MODES)) {
    const choice = makeChoice('mode', id, pickMode);
    choice.title.textContent = MODES[id].title;
    choice.meta.textContent = MODES[id].note;
    choices.mode[id] = choice;
    ui.modes.append(choice.label);
  }
}

function renderBuilds() {
  for (const [id, info] of Object.entries(CHANNELS)) {
    const choice = choices.build[id];
    const rel = release(id);
    choice.title.replaceChildren(info.title);
    if (rel) choice.title.append(h('span', { class: 'choice-tag', text: rel.tag }));
    let note = info.note;
    let available = true;
    if (!rel) {
      note = 'No release published.';
      available = false;
    } else if (!state.board) {
      note = `${shortDate(rel.published)} · ${info.note}`;
      available = false;
    } else if (!rel.boards[state.board]) {
      note = `${rel.tag} has no build for the ${boardName(state.board)}.`;
      available = false;
    } else {
      note = `${shortDate(rel.published)} · ${info.note}`;
    }
    choice.meta.textContent = note;
    choice.meta.classList.toggle('choice-warn', id === 'dev' && available);
    choice.input.disabled = !available;
    choice.input.checked = state.build === id;
  }
  const file = choices.build.file;
  file.title.textContent = 'A file from my computer';
  file.meta.textContent = 'A MeshPunk image you downloaded or built yourself.';
  file.input.disabled = !state.board;
  file.input.checked = state.build === 'file';

  ui.filePanel.hidden = state.build !== 'file';
  if (state.build === 'file') {
    const verdict = fileVerdict();
    ui.fileInfo.textContent = verdict.text;
    ui.fileInfo.classList.toggle('bad', Boolean(verdict.bad));
    ui.fileInfo.classList.toggle('muted', !verdict.bad);
  }

  const rel = state.build && state.build !== 'file' ? release(state.build) : null;
  ui.notes.hidden = !rel;
  if (rel) {
    ui.notesTag.textContent = rel.tag;
    ui.notesText.textContent = rel.notes.trim() || 'This release has no notes.';
    ui.notesLink.href = rel.url;
  }
}

function renderModes() {
  const isFile = state.build === 'file';
  ui.modes.hidden = isFile;
  ui.fileMode.hidden = !isFile;
  for (const [id, choice] of Object.entries(choices.mode)) {
    choice.input.disabled = !state.build || isFile;
    choice.input.checked = state.mode === id;
  }
  if (isFile) {
    const kind = fileVerdict().ok ? state.file.kind : null;
    ui.fileMode.textContent =
      kind === 'full'
        ? 'A full flash image is installed like a Fresh install: everything on the device is erased.'
        : kind === 'app'
          ? 'An app image is installed like an Update: the firmware is replaced, your data is kept.'
          : 'The install type follows from the file.';
  }
}

function renderAction() {
  const job = currentJob();
  if (!job) {
    ui.summary.className = 'summary muted';
    ui.summary.textContent =
      state.build === 'file' ? 'Choose a file that can be flashed.' : 'Pick a device, a build and an install type.';
    ui.flashBtn.textContent = canFlash ? 'Flash' : 'This browser cannot flash';
  } else {
    ui.summary.className = 'summary';
    ui.summary.replaceChildren(
      `${job.what} for the ${job.boardName}. `,
      job.erases
        ? h('span', { class: 'warn', text: 'Everything on the device is erased.' })
        : 'The firmware is replaced; your data is kept.',
    );
    ui.flashBtn.textContent = canFlash ? job.button : 'This browser cannot flash';
  }
  ui.flashBtn.disabled = !job || !canFlash || state.busy;
  ui.allPorts.disabled = state.busy;
  ui.selection.disabled = state.busy;
}

function fileLink(asset) {
  return h('li', null, h('a', { href: asset.url, text: asset.name }), ' ', h('span', { class: 'size', text: megabytes(asset.size) }));
}

function renderDownloads() {
  const launcher = [];
  for (const channel of Object.keys(CHANNELS)) {
    for (const asset of release(channel)?.downloads ?? []) {
      if (asset.name.endsWith('-launcher.bin')) launcher.push(fileLink(asset));
    }
  }
  ui.launcherFiles.replaceChildren(...launcher);

  const rel = state.build && state.build !== 'file' ? release(state.build) : null;
  if (rel && state.board) {
    const mine = rel.downloads.filter((asset) => asset.name.startsWith(`meshpunk-${state.board}-`));
    ui.downloads.replaceChildren(
      ...mine.map(fileLink),
      h('li', null, h('a', { href: rel.url, text: `Everything in ${rel.tag} on GitHub` })),
    );
  } else {
    ui.downloads.replaceChildren(
      ...Object.keys(CHANNELS)
        .filter((channel) => release(channel))
        .map((channel) => h('li', null, h('a', { href: release(channel).url, text: `${CHANNELS[channel].title} ${release(channel).tag} on GitHub` }))),
    );
  }
}

function render() {
  for (const [slug, choice] of Object.entries(choices.board)) choice.input.checked = state.board === slug;
  renderBuilds();
  renderModes();
  renderAction();
  renderDownloads();
}

// ---- selection ------------------------------------------------------------------

function pickBoard(slug) {
  state.board = slug;
  if (state.build !== 'file' && !boardBuild(state.build, slug)) {
    state.build = boardBuild('stable', slug) ? 'stable' : null;
  }
  render();
}

function pickBuild(id) {
  state.build = id;
  render();
}

function pickMode(id) {
  state.mode = id;
  render();
}

async function pickFile() {
  const file = ui.file.files[0];
  if (!file) {
    state.file = null;
  } else if (file.size > MAX_FILE_BYTES) {
    state.file = { name: file.name, tooLarge: true };
  } else {
    const bytes = new Uint8Array(await file.arrayBuffer());
    const shape = classifyImage(bytes);
    state.file = { name: file.name, bytes, kind: shape.kind, reason: shape.reason, tag: findBoardTag(bytes) };
  }
  render();
}

// ---- running a flash ----------------------------------------------------------

function log(text, endLine) {
  logText += endLine ? `${text}\n` : text;
  ui.log.textContent = logText;
  ui.log.scrollTop = ui.log.scrollHeight;
}

function showStages(stages) {
  ui.stages.replaceChildren(...stages.map((stage) => h('li', { 'data-stage': stage, text: STAGE_LABELS[stage] })));
}

function markStage(stage, outcome) {
  let reached = false;
  for (const item of ui.stages.children) {
    const here = item.dataset.stage === stage;
    item.className = here ? outcome : reached ? '' : 'done';
    if (here) reached = true;
  }
}

function showResult(kind, text, hint) {
  ui.result.className = `result ${kind}`;
  ui.result.replaceChildren(text);
  if (hint) ui.result.append(h('span', { class: 'hint', text: hint }));
}

function failureHint(stage, job) {
  switch (stage) {
    case 'download':
      return 'Nothing was written to the device.';
    case 'connect':
      return 'Nothing was written. If the device keeps failing to connect, put it into download mode and try again.';
    case 'write':
    case 'verify':
      return job.erases
        ? 'Flashing did not finish, so the device may not start. Flash again; if it keeps failing, try another USB cable or port.'
        : 'The update did not finish, so the device may not start. Run it again; if it keeps failing, try another USB cable or port.';
    case 'restart':
      return 'The firmware was written and verified. Press RST, or switch the device off and on, to start it.';
    default:
      return '';
  }
}

function successHint(job) {
  const dark = 'If the screen stays dark, press RST once or switch the device off and on.';
  return job.erases
    ? `The device restarts by itself. The first start unpacks the bundled files, which takes about a minute before the home screen appears. ${dark}`
    : `The device restarts by itself with your settings and messages kept. The first start refreshes the bundled files, which takes about a minute. ${dark}`;
}

function setBusy(busy) {
  state.busy = busy;
  render();
}

async function choosePort(board) {
  const options = {};
  if (!ui.allPorts.checked) {
    options.filters = [{ usbVendorId: parseInt(board.usb.vid, 16), usbProductId: parseInt(board.usb.pid, 16) }];
  }
  return navigator.serial.requestPort(options);
}

async function startFlash() {
  const job = currentJob();
  if (!job || state.busy) return;
  const board = state.index.boards[state.board];

  // The port chooser needs this click's user activation, so it comes before
  // the download.
  let port;
  try {
    port = await choosePort(board);
  } catch (error) {
    ui.run.hidden = false;
    ui.stages.replaceChildren();
    ui.bar.value = 0;
    if (error.name === 'NotFoundError') {
      showResult('', 'No port was chosen.', 'If the device is not in the list, put it into download mode and try again.');
    } else {
      showResult('bad', `The browser did not open the port chooser: ${error.message}`);
    }
    return;
  }

  const stages = STAGES.filter((stage) => stage !== 'download' || job.file);
  logText = '';
  ui.log.textContent = '';
  ui.copyLog.textContent = 'Copy log';
  ui.run.hidden = false;
  ui.bar.value = 0;
  showStages(stages);
  showResult('', 'Working. Keep the device connected and this tab open.');
  setBusy(true);

  let stage = stages[0];
  const onStage = (next) => {
    stage = next;
    markStage(next, 'active');
    if (next === 'connect') ui.bar.value = 0;
  };
  const onProgress = (fraction) => {
    ui.bar.value = fraction;
  };

  try {
    let image = job.bytes;
    if (job.file) {
      onStage('download');
      log(`Downloading ${job.file.path}`, true);
      image = await downloadImage(job.file, SITE_ROOT, { fetch: (url, options) => fetch(url, options), onProgress });
      log(`Downloaded ${image.length} bytes, SHA-256 matches.`, true);
    }
    await flashDevice(
      { image, kind: job.kind, chip: job.chip, boardName: job.boardName, layouts: job.layouts },
      {
        createLoader: (terminal) => {
          const transport = new Transport(port, false);
          const loader = new ESPLoader({ transport, baudrate: BAUD, terminal, debugLogging: false });
          return { loader, transport };
        },
        onStage,
        onProgress,
        log,
      },
    );
    for (const item of ui.stages.children) item.className = 'done';
    ui.bar.value = 1;
    showResult('good', `Done. ${job.what} is on the ${job.boardName}.`, successHint(job));
  } catch (error) {
    const failedAt = error.stage ?? stage;
    markStage(failedAt, 'failed');
    log(`Stopped at "${STAGE_LABELS[failedAt]}": ${error.message}`, true);
    showResult('bad', `Stopped at "${STAGE_LABELS[failedAt]}": ${error.message}`, failureHint(failedAt, job));
    ui.logBox.open = true;
  } finally {
    setBusy(false);
  }
}

// ---- start ----------------------------------------------------------------------

async function start() {
  ui.gate.hidden = canFlash;
  try {
    const response = await fetch(new URL('firmware/index.json', SITE_ROOT), { cache: 'no-store' });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    state.index = await response.json();
  } catch (error) {
    ui.loadError.hidden = false;
    ui.loadError.textContent = `The list of builds (firmware/index.json) could not be loaded: ${error.message}`;
    return;
  }

  buildChoices();
  render();
  ui.form.hidden = false;
  ui.more.hidden = false;

  ui.form.addEventListener('submit', (event) => event.preventDefault());
  ui.file.addEventListener('change', pickFile);
  ui.flashBtn.addEventListener('click', startFlash);
  ui.copyLog.addEventListener('click', async () => {
    try {
      await navigator.clipboard.writeText(logText);
      ui.copyLog.textContent = 'Copied';
    } catch (error) {
      ui.copyLog.textContent = `Not copied: ${error.message}`;
    }
  });
  window.addEventListener('beforeunload', (event) => {
    if (!state.busy) return;
    event.preventDefault();
    event.returnValue = '';
  });
}

start();
