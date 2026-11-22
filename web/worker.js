// The build, off the main thread.
//
// Everything happens here: the vendor's image is read, its rootfs is unpacked,
// this port's files go in, the stock libraries that nothing uses any more are
// dropped, mkfs.ubifs (WebAssembly) packs a new rootfs, the checksums in
// UPDATE.TXT are recomputed and the ISO is written. No network request carries the
// user's file anywhere; the only thing fetched is this port's own payload and the
// wasm module.
//
// The long-running loops post progress messages so the page can show where it is;
// the tree never leaves this worker.

import { buildIso, readFirmwareIso } from './lib/iso9660.js';
import { installTree, trimStock, updateDigests } from './lib/install.js';
import { firmwareMkfsArgs, makeLzoDecompress, mountTree, runMkfs } from './lib/mkfs-ubifs.js';
import { md5 } from './lib/md5.js';
import { loadPayload } from './lib/payload.js';
import { UbifsVolume } from './lib/ubifs.js';
import { Vfs } from './lib/vfs.js';
import createMkfsUbifs from './wasm/mkfs_ubifs.js';

/** The stock bootloader's digest: the one file whose contents must be the vendor's. */
const STOCK_UBOOT_MD5 = '1f54858d6b4e6f482a81a3378c0e5bf8';

const say = (step, detail) => self.postMessage({ type: 'step', step, detail });
const progress = (fraction, total = 0, loaded = 0) => self.postMessage({ type: 'progress', fraction, total, loaded });

async function sha256(bytes) {
  const digest = await crypto.subtle.digest('SHA-256', bytes);
  return [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, '0')).join('');
}

async function build({ file, payloadUrl }) {
  say('payload', 'Fetching the port’s binaries and assets');
  const payload = await loadPayload(payloadUrl, {
    onProgress: ({ loaded, total }) => progress(loaded / total / 8, total, loaded),
  });
  const { version, build_stamp: buildStamp } = payload.meta;

  say('read', 'Reading the vendor’s image');
  const bytes = new Uint8Array(await file.arrayBuffer());
  let iso;
  try {
    iso = readFirmwareIso(bytes);
  } catch (error) {
    throw new Error(`this is not an ISO9660 firmware image: ${error.message}`);
  }
  const get = (name) => {
    const entry = iso.files.get(name);
    if (!entry) throw new Error(`the image has no ${name}: it is not a v3_analog firmware`);
    return entry.data;
  };
  const uboot = get('UBOOT.BIN');
  const ubootDigest = md5(uboot);
  if (ubootDigest !== STOCK_UBOOT_MD5) {
    throw new Error(
      `this image's UBOOT.BIN is ${ubootDigest}, not the V3 Analog's stock bootloader. ` +
        'It is another model’s firmware, or a vendor version newer than the one this port is built on.',
    );
  }
  const system = get('SYSTEM.UBI');

  say('unpack', 'Unpacking the rootfs');
  progress(0.2);
  const module = await createMkfsUbifs({
    locateFile: (path) => new URL(`wasm/${path}`, self.location.href).href,
    print: () => {},
    printErr: (line) => self.postMessage({ type: 'log', line }),
  });
  const decompress = makeLzoDecompress(module);
  const volume = new UbifsVolume(system, { lzoDecompress: decompress });
  if (volume.badCrc !== 0) throw new Error(`${volume.badCrc} UBIFS nodes are corrupt in this image`);
  const vfs = Vfs.fromUbifs(volume);
  progress(0.35);

  say('install', 'Installing the player, the launcher and the assets');
  for (const line of installTree(vfs, { payload, buildStamp, osVersion: version })) {
    self.postMessage({ type: 'log', line });
  }
  progress(0.5);

  say('trim', 'Dropping the stock libraries nothing uses');
  for (const line of trimStock(vfs).lines) self.postMessage({ type: 'log', line });
  progress(0.6);

  say('pack', 'Packing the rootfs (mkfs.ubifs in WebAssembly)');
  mountTree(module, vfs);
  const rc = runMkfs(module, firmwareMkfsArgs('/out.ubi'));
  if (rc !== 0) throw new Error(`mkfs.ubifs failed with ${rc}: the tree does not fit the volume`);
  const image = module.FS.readFile('/out.ubi');
  progress(0.8);

  say('write', 'Writing the checksums and the ISO');
  const digests = updateDigests(new TextDecoder().decode(get('UPDATE.TXT')), {
    'SYSTEM.UBI': image,
    'UIMAGE.BIN': get('UIMAGE.BIN'),
  });
  for (const line of digests.lines) self.postMessage({ type: 'log', line });

  const out = buildIso([
    { name: 'SYSTEM.UBI', data: image },
    { name: 'UIMAGE.BIN', data: get('UIMAGE.BIN') },
    { name: 'UBOOT.BIN', data: uboot },
    { name: 'UPDATE.TXT', data: new TextEncoder().encode(digests.text) },
    { name: 'VERSION.TXT', data: get('VERSION.TXT') },
    { name: '_GITIGNO', data: get('_GITIGNO') },
  ]);
  progress(0.95);

  const summary = vfs.summary();
  return {
    blob: new Blob([out], { type: 'application/octet-stream' }),
    size: out.length,
    sha256: await sha256(out),
    version,
    buildStamp,
    files: summary,
    payloadFiles: payload.summary().files,
  };
}

self.onmessage = async (event) => {
  const { file, payloadUrl } = event.data;
  try {
    const result = await build({ file, payloadUrl });
    progress(1);
    self.postMessage({ type: 'done', ...result });
  } catch (error) {
    // The stack goes along: the page shows the sentence, the console shows where.
    self.postMessage({ type: 'error', message: error?.message ?? String(error), stack: error?.stack });
  }
};
