// The page: a drop zone, a step list, and a download link.
//
// All the work is in worker.js; this file only moves the file across, shows where
// the build is, and hands back the result. The worker is started on the first drop
// and reused afterwards, because loading the wasm module again costs more than the
// build does.

const STEPS = [
  ['payload', 'Fetch the port’s binaries and assets'],
  ['read', 'Read the vendor’s image'],
  ['unpack', 'Unpack the rootfs'],
  ['install', 'Install the player and the assets'],
  ['trim', 'Drop the stock libraries'],
  ['pack', 'Pack the rootfs'],
  ['write', 'Write the checksums and the image'],
];

import { defaultVersion, loadVersions, versionLabel } from './lib/versions.js';

const drop = document.getElementById('drop');
const versionSelect = document.getElementById('version');
const versionPicker = document.getElementById('version-picker');
const versionInfo = document.getElementById('version-info');
const fileInput = document.getElementById('file');
const stepsList = document.getElementById('steps');
const progressSection = document.getElementById('progress');
const barFill = document.getElementById('bar-fill');
const message = document.getElementById('message');
const notes = document.getElementById('notes');
const inputName = document.getElementById('input-name');
const inputDetail = document.getElementById('input-detail');
const resultSection = document.getElementById('result');
const resultTitle = document.getElementById('result-title');
const resultDetail = document.getElementById('result-detail');
const download = document.getElementById('download');
const errorSection = document.getElementById('error');
const errorText = document.getElementById('error-text');
const payloadVersion = document.getElementById('payload-version');

let worker = null;
let objectUrl = null;
let running = false;
/** @type {Array<object>} */
let versions = [];
let version = null;

async function loadVersionList() {
  try {
    versions = await loadVersions('versions.json');
  } catch (error) {
    // A page opened without its deployment (a checkout of web/ served by hand) has
    // no versions.json; the build it makes is still the one worth offering.
    console.warn(`versions.json unavailable (${error.message}); offering this build only`);
    versions = [{ label: 'this page', url: 'payload/current.tar.gz', release: false }];
  }
  versionSelect.replaceChildren(
    ...versions.map((entry, index) => {
      const option = document.createElement('option');
      option.value = String(index);
      option.textContent = versionLabel(entry);
      return option;
    }),
  );
  // One version is not a choice: show it as a line rather than as a list of one.
  // None at all is a state the site can be in (no released payload yet) and the
  // page says so instead of offering a build it does not have.
  versionPicker.hidden = versions.length <= 1;
  versionSelect.disabled = versions.length === 0;
  if (versions.length === 0) {
    versionInfo.textContent = 'No released version is published yet: nothing to build with.';
    drop.setAttribute('aria-disabled', 'true');
  }
  selectVersion(defaultVersion(versions));
}

function selectVersion(entry) {
  version = entry;
  if (!entry) return;
  const index = versions.indexOf(entry);
  if (index >= 0) versionSelect.value = String(index);
  versionInfo.textContent = [
    entry.stamp ? `player built ${entry.stamp}` : null,
    entry.sha256 ? `payload sha256 ${entry.sha256.slice(0, 12)}…` : null,
  ]
    .filter(Boolean)
    .join(' · ');
}

function resetSteps() {
  stepsList.replaceChildren(
    ...STEPS.map(([id, label]) => {
      const item = document.createElement('li');
      item.dataset.step = id;
      item.textContent = label;
      return item;
    }),
  );
}

function markStep(id, detail) {
  for (const item of stepsList.children) {
    if (item.dataset.step === id) item.classList.add('active');
  }
  if (detail) message.textContent = detail;
}

function doneStep(id) {
  for (const item of stepsList.children) {
    if (item.dataset.step === id) {
      item.classList.remove('active');
      item.classList.add('done');
    }
  }
}

function showError(text) {
  running = false;
  drop.setAttribute('aria-busy', 'false');
  progressSection.hidden = true;
  errorSection.hidden = false;
  errorText.textContent = text;
}

function startWorker() {
  if (worker) return worker;
  worker = new Worker('worker.js', { type: 'module' });
  worker.onmessage = (event) => {
    const data = event.data;
    if (data.type === 'step') {
      markStep(data.step, data.detail);
    } else if (data.type === 'progress') {
      barFill.style.width = `${Math.max(2, Math.round(data.fraction * 100))}%`;
    } else if (data.type === 'log') {
      message.textContent = data.line;
    } else if (data.type === 'done') {
      finish(data);
    } else if (data.type === 'error') {
      if (data.stack) console.error(data.stack);
      showError(data.message);
    }
  };
  worker.onerror = (event) => showError(event.message || 'the build worker stopped');
  return worker;
}

function finish({ blob, size, sha256, version: imageVersion, buildStamp, files, payloadFiles }) {
  running = false;
  drop.setAttribute('aria-busy', 'false');
  for (const [id] of STEPS) doneStep(id);
  barFill.style.width = '100%';
  message.textContent = '';

  if (objectUrl) URL.revokeObjectURL(objectUrl);
  objectUrl = URL.createObjectURL(blob);
  download.href = objectUrl;

  resultSection.hidden = false;
  resultTitle.textContent = 'The image is ready';
  resultDetail.textContent = [
    `${(size / 1048576).toFixed(1)} MB`,
    `rootfs ${files.file} files, ${files.dir} directories, ${(files.bytes / 1048576).toFixed(1)} MB`,
    `sha256 ${sha256}`,
  ].join(' · ');
  payloadVersion.textContent =
    `built from ${versionLabel(version)}: image ${imageVersion}, stamp ${buildStamp}, ${payloadFiles} files`;
  resultSection.scrollIntoView({ behavior: 'smooth', block: 'start' });
}

async function handleFile(file) {
  if (running) return;
  if (!version) {
    showError(
      versions.length === 0
        ? 'No released version is published yet, so there is nothing to build the image with.'
        : 'the list of versions is still loading; try again in a moment',
    );
    return;
  }
  errorSection.hidden = true;
  resultSection.hidden = true;
  progressSection.hidden = false;
  resetSteps();
  barFill.style.width = '2%';
  message.textContent = '';
  drop.setAttribute('aria-busy', 'true');
  running = true;

  inputName.textContent = `${file.name} (${(file.size / 1048576).toFixed(1)} MB)`;
  inputDetail.textContent = `Read in this browser; nothing is uploaded. Version: ${versionLabel(version)}.`;
  notes.hidden = false;

  startWorker().postMessage({ file, version });
}

versionSelect.addEventListener('change', () => {
  selectVersion(versions[Number(versionSelect.value)]);
});

drop.addEventListener('click', () => {
  if (!running) fileInput.click();
});

drop.addEventListener('keydown', (event) => {
  if ((event.key === 'Enter' || event.key === ' ') && !running) {
    event.preventDefault();
    fileInput.click();
  }
});

fileInput.addEventListener('change', () => {
  const file = fileInput.files?.[0];
  if (file) handleFile(file);
});

loadVersionList();

for (const type of ['dragenter', 'dragover']) {
  drop.addEventListener(type, (event) => {
    event.preventDefault();
    drop.classList.add('over');
  });
}

for (const type of ['dragleave', 'drop']) {
  drop.addEventListener(type, (event) => {
    event.preventDefault();
    drop.classList.remove('over');
  });
}

drop.addEventListener('drop', (event) => {
  const file = event.dataTransfer?.files?.[0];
  if (file) handleFile(file);
});
