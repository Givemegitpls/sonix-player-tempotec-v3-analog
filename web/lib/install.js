// What the packer does to the tree, in the browser.
//
// Three scripts, ported step for step so that a build here and a build with
// `sonix-packer/v3_analog_packer.sh` produce the same tree:
//
//   installTree   sonix-packer/tools/install_tree.sh
//   trimStock     sonix-packer/tools/trim_stock.py
//   updateDigests sonix-packer/tools/fix_update_md5.py
//
// They are ports, not reimplementations: same file list, same checks, same order,
// same failures. Where the shell used `sed -i` on a text file, this rewrites the
// text and keeps the file's mode -- the exception that bit the port once already
// (a rewrite that drops the executable bit leaves an image whose init silently
// skips the launcher, which reads as a hang on the boot logo).
//
// Anything that fails here throws: the page must not hand out an image whose
// Bluetooth userland is the stock one, or whose libraries do not resolve.

import { md5 } from './md5.js';

const STOCK_PLAYER = 'usr/bin/hiby_player';
const STOCK_LAUNCHER = 'usr/bin/hiby_player.sh';
const INIT_SCRIPT = 'etc/init.d/S92_03_start_music_player';
const SYSTEM_INFO = 'usr/resource/sonix/components/system-info.json';

/** The stock surface nobody reads once the stock player is gone (install_tree.sh step 4). */
const STOCK_RESOURCES = ['usr/resource/litegui', 'usr/resource/layout', 'usr/resource/str', 'usr/resource/fonts'];

/** What the port adds and the stock image does not carry (install_tree.sh step 5). */
const REQUIRED = [
  'usr/bin/sonix_player',
  'usr/bin/bluealsa',
  'usr/sbin/bluetoothd',
  'usr/lib/libbluetooth.so.3',
  'usr/lib/libfdk-aac.so.2',
  'usr/lib/libopenaptx.so.0',
  'usr/lib/libldacBT_enc.so.2',
  'usr/lib/libldacBT_abr.so.2',
  'usr/lib/alsa-lib/libasound_module_pcm_bluealsa.so',
  'usr/share/alsa/alsa.conf.d/20-bluealsa.conf',
  'usr/etc/bluetooth/main.conf',
  'etc/dbus-1/system.d/sonix-player.conf',
  'usr/resource/sonix/fonts/default.otf',
  SYSTEM_INFO,
];

/** The mode lists at the end of install_tree.sh. */
const EXECUTABLE = [
  'usr/bin/sonix_player',
  'usr/bin/sonix_player.sh',
  'usr/bin/bluealsa',
  'usr/sbin/bluetoothd',
  'usr/bin/bt_init',
  INIT_SCRIPT,
];
const MODE_755 = ['usr/lib/libbluetooth.so.3'];
const MODE_644 = [
  'usr/lib/libfdk-aac.so.2',
  'usr/lib/libopenaptx.so.0',
  'usr/lib/libldacBT_enc.so.2',
  'usr/lib/libldacBT_abr.so.2',
  'usr/lib/alsa-lib/libasound_module_pcm_bluealsa.so',
];

/** trim_stock.py: path under the tree -> why it is dead. */
export const DEAD = {
  'usr/lib/libFLAC.so': "stock player's FLAC decoder (hiby_player is deleted)",
  'usr/lib/libFLAC.so.8': "stock player's FLAC decoder (hiby_player is deleted)",
  'usr/lib/libmp3lame.so': "stock player's MP3 encoder/decoder",
  'usr/lib/libmp3lame.so.0': "stock player's MP3 encoder/decoder",
  'usr/lib/libwma.so': "stock player's WMA decoder",
  'usr/lib/libaac.so': "stock player's AAC decoder",
  'usr/lib/libmp3.so': "stock player's MP3 decoder",
  'usr/lib/libdsd2pcm.so': "stock player's DSD-to-PCM converter",
  'usr/lib/libmad.so.0': "stock player's MPEG audio decoder",
  'usr/lib/libSDL-1.2.so.0': 'nothing in this firmware uses SDL',
  'usr/lib/libglib-2.0.so.0.4000.0': 'stock glib; BlueZ 5 and the player use 2.46.1',
};

/** Removed only when the /lib copy is there: the loader resolves it first. */
export const DUPLICATES = {
  'usr/lib/libcrypto.so.1.0.0': 'lib/libcrypto.so.1.0.0',
  'usr/lib/libssl.so.1.0.0': 'lib/libssl.so.1.0.0',
};

const encoder = new TextEncoder();
const decoder = new TextDecoder();

const basename = (path) => path.slice(path.lastIndexOf('/') + 1);

/**
 * The values written into system-info.json reach a regex and a JSON string, and
 * the shell version refuses the characters that would break either. A browser
 * build takes its version from the payload and its stamp from the player binary's
 * mtime, so a bad one is a broken payload, not a user's typo -- but the check
 * stays, because a silently mangled version is what the packer's comment warns
 * about.
 */
function stringSafe(name, value) {
  if (/[\\&|"`\n]/.test(value)) {
    throw new Error(`${name} holds a backslash, &, |, a quote, a backtick or a newline: ${JSON.stringify(value)}`);
  }
  return value;
}

/** sed's s|("key"\s*:\s*")[^"]*(")|\1value\2| on a text file, keeping its mode. */
function rewriteJsonValue(vfs, path, key, value, label) {
  const node = vfs.require(path);
  const text = decoder.decode(node.read());
  const pattern = new RegExp(`("${key}"\\s*:\\s*")[^"]*(")`);
  if (!pattern.test(text)) throw new Error(`no "${key}" in ${path}`);
  const rewritten = text.replace(pattern, (_, before, after) => `${before}${value}${after}`);
  node.setContent(encoder.encode(rewritten));
  return label;
}

/**
 * install_tree.sh: the player, the launcher, the assets, and the stock surface
 * they replace.
 *
 * @param {import('./vfs.js').Vfs} vfs the rootfs, from the vendor image
 * @param {{payload: import('./payload.js').Payload, buildStamp: string, osVersion: string}} inputs
 */
export function installTree(vfs, { payload, buildStamp, osVersion }) {
  const log = [];
  stringSafe('build_version', buildStamp);
  stringSafe('OS_version', osVersion);
  if (!buildStamp) throw new Error('build_version is empty');

  // 1. The player, in the stock player's place.
  vfs.remove(STOCK_PLAYER);
  vfs.writeFile('usr/bin/sonix_player', payload.player, 0o775);
  log.push(`usr/bin/sonix_player (${payload.player.length} bytes, 775)`);

  // 2. The launcher, and the init script that names it. One substitution covers
  //    both spellings: hiby_player.sh becomes sonix_player.sh.
  vfs.remove(STOCK_LAUNCHER);
  const init = vfs.require(INIT_SCRIPT);
  const rewritten = decoder.decode(init.read()).replaceAll('hiby_player', 'sonix_player');
  if (rewritten.includes('hiby_player')) throw new Error('hiby_player survived the rewrite of the init script');
  init.setContent(encoder.encode(rewritten));
  log.push(`${INIT_SCRIPT}: hiby_player -> sonix_player`);

  vfs.writeFile('usr/bin/sonix_launch', payload.launcher, 0o775);
  log.push(`usr/bin/sonix_launch (${payload.launcher.length} bytes, 775)`);

  // 3. The assets: the Blaze port's interface, then this device's files over it.
  for (const root of payload.meta.assets) {
    payload.overlay(vfs, root);
    log.push(`${root} unpacked over the tree`);
  }

  // 4. The build stamp and the version string the player shows in About.
  rewriteJsonValue(vfs, SYSTEM_INFO, 'build_version', buildStamp, `build_version = ${buildStamp}`);
  rewriteJsonValue(vfs, SYSTEM_INFO, 'OS_version', osVersion, `OS_version = ${osVersion}`);
  log.push(`build_version = ${buildStamp}`);
  log.push(`OS_version = ${osVersion}`);

  // 5. The stock interface's resources go; usr/resource/sonix stays, and so does
  //    usr/resource/bt_name (the adapter's name, which the port reads).
  for (const path of STOCK_RESOURCES) if (vfs.remove(path)) log.push(`${path} removed`);

  // 6. What the port needs and the stock firmware does not have.
  for (const path of REQUIRED) {
    if (!vfs.find(path)) throw new Error(`${path} did not land in the tree`);
  }
  const launch = vfs.find('usr/bin/sonix_launch');
  if (!launch || (launch.mode & 0o111) === 0) throw new Error('usr/bin/sonix_launch is not executable');

  const info = decoder.decode(vfs.require(SYSTEM_INFO).read());
  if (!info.includes('"TempoTec V3 Analog"')) throw new Error('system-info.json does not name the V3 Analog');

  // 7. Ownership and modes, the way mkfs.ubifs and the boot want them. The stock
  //    tree is 1004:1004 throughout and the image carries those numbers.
  for (const node of vfs.walk()) {
    node.uid = 1004;
    node.gid = 1004;
  }
  for (const path of EXECUTABLE) vfs.chmod(path, 0o775);
  for (const path of MODE_755) vfs.chmod(path, 0o755);
  for (const path of MODE_644) vfs.chmod(path, 0o644);

  return log;
}

/** A file or symlink by path, for the trim lists, that reports its size and why. */
function remove(vfs, path, why) {
  const node = vfs.find(path);
  if (!node) return null;
  const size = node.kind === 'symlink' ? encoder.encode(node.target ?? '').length : node.size;
  vfs.remove(path);
  return { size, line: `- ${path} (${(size / 1048576).toFixed(2)} MB): ${why}` };
}

/** The unversioned dev symlinks in usr/lib that point at `name`. */
function devSymlinksTo(vfs, name) {
  const lib = vfs.find('usr/lib');
  const found = [];
  if (!lib?.children) return found;
  for (const [entryName, node] of lib.children) {
    if (node.kind === 'symlink' && basename(node.target) === name) found.push(`usr/lib/${entryName}`);
  }
  return found;
}

/**
 * trim_stock.py: removes the stock player's decoders and one duplicated library,
 * then proves nothing is left referring to them.
 *
 * Two checks stand behind it, and both fail the build: no remaining file may name
 * a removed soname, and no symlink under usr/lib or lib may point at nothing. The
 * reference scan is the same substring search the Python does -- an ELF's NEEDED
 * entry is a C string in the file, and a full ELF parse would be a different tool.
 */
export function trimStock(vfs) {
  if (!vfs.find('usr')) throw new Error('the tree is not an unpacked rootfs');
  const lines = [];
  const removedNames = new Set();
  let freed = 0;

  for (const [path, why] of Object.entries(DEAD)) {
    const removed = remove(vfs, path, why);
    if (!removed) continue;
    freed += removed.size;
    lines.push(removed.line);
    // The soname, not the file name: a NEEDED entry says "libFLAC.so.8" whatever
    // the file behind it is called.
    removedNames.add(basename(path));
  }

  // A library can be split over several files (libFLAC.so.8 -> libFLAC.so.8.3.0);
  // the rest of the family goes with it, or the megabytes it holds are still here.
  const lib = vfs.find('usr/lib');
  const sonames = Object.keys(DEAD).map(basename);
  if (lib?.children) {
    for (const name of [...lib.children.keys()].sort()) {
      const path = `usr/lib/${name}`;
      if (DEAD[path]) continue;
      const soname = sonames.find((candidate) => name.startsWith(`${candidate}.`));
      if (!soname) continue;
      const removed = remove(vfs, path, `part of the family: ${DEAD[`usr/lib/${soname}`]}`);
      if (!removed) continue;
      freed += removed.size;
      lines.push(removed.line);
    }
  }

  for (const [path, twin] of Object.entries(DUPLICATES)) {
    const twinNode = vfs.find(twin);
    if (!twinNode || twinNode.kind === 'symlink') {
      lines.push(`= ${path} kept: ${twin} is not there, and it is the one in use`);
      continue;
    }
    const removed = remove(vfs, path, `duplicate of ${twin}`);
    if (!removed) continue;
    freed += removed.size;
    lines.push(removed.line);
    // The dev symlinks pointing at it have to go: their target is gone.
    for (const link of devSymlinksTo(vfs, basename(path))) {
      const linkRemoved = remove(vfs, link, `dangling link to the duplicate ${basename(path)}`);
      if (!linkRemoved) continue;
      freed += linkRemoved.size;
      lines.push(linkRemoved.line);
    }
  }

  // 1. No remaining file may mention a soname that was just removed.
  const hits = [];
  for (const node of vfs.walk()) {
    if (node.kind !== 'file' || node.size === 0) continue;
    const data = node.read();
    for (const soname of removedNames) {
      if (indexOfAscii(data, soname) >= 0) hits.push(`${node.path} -> ${soname}`);
    }
  }
  if (hits.length > 0) {
    throw new Error(`these files still need a library that was just removed:\n  ${hits.join('\n  ')}`);
  }

  // 2. No symlink under usr/lib or lib may point at nothing.
  const dangling = [];
  for (const root of ['usr/lib', 'lib']) {
    const base = vfs.find(root);
    if (!base) continue;
    for (const node of vfs.walk(base)) {
      if (node.kind !== 'symlink') continue;
      if (!resolveExists(vfs, node)) dangling.push(node.path);
    }
  }
  if (dangling.length > 0) throw new Error(`these symlinks point at nothing:\n  ${dangling.join('\n  ')}`);

  lines.push(`trimmed ${(freed / 1048576).toFixed(2)} MB of stock libraries nothing uses`);
  return { lines, freed, removed: [...removedNames] };
}

/** indexOf for a byte array and an ASCII needle. */
function indexOfAscii(haystack, needle) {
  const first = needle.charCodeAt(0);
  for (let i = 0; i + needle.length <= haystack.length; i += 1) {
    if (haystack[i] !== first) continue;
    let match = true;
    for (let j = 1; j < needle.length; j += 1) {
      if (haystack[i + j] !== needle.charCodeAt(j)) {
        match = false;
        break;
      }
    }
    if (match) return i;
  }
  return -1;
}

/** Whether a symlink resolves inside the tree (an absolute target is the root). */
function resolveExists(vfs, node) {
  const target = node.target ?? '';
  const path = target.startsWith('/') ? target : `${node.parent.path}/${target}`;
  return resolveNode(vfs, path, 0) !== undefined;
}

function resolveNode(vfs, path, depth) {
  if (depth > 32) return undefined;
  const node = vfs.find(path);
  if (!node) return undefined;
  if (node.kind !== 'symlink') return node;
  const target = node.target.startsWith('/') ? node.target : `${node.parent.path}/${node.target}`;
  return resolveNode(vfs, target, depth + 1);
}

/**
 * fix_update_md5.py: the digests the updater checks, recomputed from the images
 * that were just packed. The kernel's block only changes when a kernel is
 * installed on purpose; rewriting it from the file that is actually in the image
 * is what keeps the stock value from being carried by accident.
 *
 * UPDATE.TXT is one of the ISO's own files, not part of the rootfs, so this works
 * on its text.
 *
 * @param {string} text the stock UPDATE.TXT
 * @param {Record<string, Uint8Array|undefined>} images file name -> contents
 * @returns {{text: string, lines: string[]}}
 */
export function updateDigests(text, images) {
  const blocks = { 'SYSTEM.UBI': 'rootfs', 'UIMAGE.BIN': 'kernel' };
  const lines = [];
  for (const [name, data] of Object.entries(images)) {
    const block = blocks[name];
    if (!block || !data) continue;
    const digest = md5(data);
    const start = text.indexOf(`${block}={`);
    if (start < 0) throw new Error(`no ${block}={...} block in UPDATE.TXT`);
    const end = text.indexOf('}', start);
    const body = text.slice(start, end);
    if (!/md5=[0-9a-fA-F]{32}/.test(body)) throw new Error(`the ${block} block has no md5= line`);
    // Not "did the text change": a stock kernel whose digest is already right
    // rewrites to itself, and that is the good case, not a failure.
    const rewritten = body.replace(/(md5=)[0-9a-fA-F]{32}/, `$1${digest}`);
    text = text.slice(0, start) + rewritten + text.slice(end);
    lines.push(`${block} md5 = ${digest}`);
  }
  if (lines.length === 0) throw new Error('nothing to do: no SYSTEM.UBI or UIMAGE.BIN among the images');
  return { text, lines };
}
