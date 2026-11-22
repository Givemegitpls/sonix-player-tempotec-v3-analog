// Checks the browser's firmware path against the packer's own result.
//
// The page reads the vendor's `.upt` itself instead of uploading it: it parses
// ISO9660, extracts the rootfs from the UBIFS image in that file, edits the tree,
// and packs a new UBIFS image with mkfs.ubifs compiled to WASM. All of that is
// plain ESM over Uint8Array (web/lib/*.js) and runs under Node unchanged, which
// is what this harness does, on a real image, against a reference tree that
// `sonix-packer/v3_analog_packer.sh` extracted with ubi_reader.
//
//   node sonix-packer/tools/webtest/run.mjs read   [options]   # ISO + UBIFS read
//   node sonix-packer/tools/webtest/run.mjs write  [options]   # read, pack, re-read
//   node sonix-packer/tools/webtest/run.mjs build  [options]   # the whole page
//
// `build` is the page's own pipeline: read the vendor image, install the player and
// the assets (as install_tree.sh does), trim the stock libraries (trim_stock.py), pack
// with mkfs.ubifs and rewrite UPDATE.TXT -- then compare the resulting tree with the
// one sonix-packer/v3_analog_packer.sh built from the same binaries. It takes the
// payload out of the repository (sonix-packer/sonix_player, sonix_launch,
// assets/BLAZE, assets/V3ANALOG) and its identity from the reference tree's
// system-info.json, so both builds write the same version strings.
//
//   --stock PATH     the vendor's image        (sonix-packer/v3_analog_original.upt)
//   --ref DIR        ubi_reader's extraction   (sonix-packer/build/SYSTEM)
//   --wasm-js PATH   built loader             (sonix-packer/build-wasm/out/mkfs_ubifs.js)
//   --extract DIR    write the tree read out of the image to DIR
//   --out PATH       write the rebuilt .upt   (write mode)
//   --payload DIR    repository directory the payload comes from  (sonix-packer)
//   --payload-archive PATH  the payload.tar.gz the page fetches (tests the tar path)
//   --no-hash        skip file contents (faster)
//
// Neither the vendor image nor the extracted tree is in git (docs/BUILD.md,
// "Distributing an image"); both are inputs to this test on a machine that has
// them.

import { createHash } from 'node:crypto';
import {
  lstatSync,
  mkdirSync,
  readdirSync,
  readFileSync,
  readlinkSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { dirname, join } from 'node:path';
import { pathToFileURL } from 'node:url';

import { buildIso, readFirmwareIso } from '../../../web/lib/iso9660.js';
import { UbifsVolume } from '../../../web/lib/ubifs.js';
import { Vfs, kindOf } from '../../../web/lib/vfs.js';
import { firmwareMkfsArgs, makeLzoDecompress, mountTree, runMkfs } from '../../../web/lib/mkfs-ubifs.js';

function parseArgs(argv) {
  const args = {
    mode: argv[2] ?? 'read',
    stock: 'sonix-packer/v3_analog_original.upt',
    ref: 'sonix-packer/build/SYSTEM',
    wasmJs: 'sonix-packer/build-wasm/out/mkfs_ubifs.js',
    payloadDir: 'sonix-packer',
    payloadArchive: null,
    extract: null,
    out: null,
    hash: true,
  };
  for (let i = 3; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === '--stock') args.stock = argv[++i];
    else if (arg === '--ref') args.ref = argv[++i];
    else if (arg === '--wasm-js') args.wasmJs = argv[++i];
    else if (arg === '--payload') args.payloadDir = argv[++i];
    else if (arg === '--payload-archive') args.payloadArchive = argv[++i];
    else if (arg === '--extract') args.extract = argv[++i];
    else if (arg === '--out') args.out = argv[++i];
    else if (arg === '--no-hash') args.hash = false;
    else if (arg === '--help' || arg === '-h') {
      console.log('usage: run.mjs read|write [--stock PATH] [--ref DIR] [--wasm-js PATH] [--extract DIR] [--out PATH] [--no-hash]');
      process.exit(0);
    } else {
      throw new Error(`unknown argument ${arg}`);
    }
  }
  if (!['read', 'write', 'build'].includes(args.mode)) throw new Error(`unknown mode ${args.mode}`);
  return args;
}

/** The wasm module, with the LZO decompressor the reader needs. */
async function loadModule(args) {
  const { default: createMkfsUbifs } = await import(pathToFileURL(args.wasmJs).href);
  const wasmBinary = readFileSync(args.wasmJs.replace(/\.js$/, '.wasm'));
  const module = await createMkfsUbifs({
    wasmBinary,
    print: () => {},
    printErr: (line) => process.stderr.write(`mkfs.ubifs: ${line}\n`),
  });
  return module;
}

/** The reference tree, as ubi_reader extracted it: kind, mode, size, contents. */
function readReference(root) {
  const entries = new Map();
  const walk = (dir, prefix) => {
    for (const name of readdirSync(dir)) {
      const path = prefix ? `${prefix}/${name}` : name;
      const full = join(dir, name);
      const st = lstatSync(full);
      const kind = st.isSymbolicLink() ? 'symlink' : st.isDirectory() ? 'dir' : 'file';
      if (kind === 'symlink') {
        entries.set(path, { kind, mode: st.mode & 0o7777, target: readlinkSync(full) });
      } else if (kind === 'dir') {
        entries.set(path, { kind, mode: st.mode & 0o7777 });
        walk(full, path);
      } else {
        entries.set(path, { kind, mode: st.mode & 0o7777, size: st.size, full });
      }
    }
  };
  walk(root, '');
  return entries;
}

const sha256 = (bytes) => createHash('sha256').update(bytes).digest('hex');

/** What our own reader reports for a volume, keyed by path. */
function readTree(volume, args) {
  const fs = Vfs.fromUbifs(volume);
  const entries = new Map();
  for (const node of fs.walk()) {
    const path = node.path;
    const kind = kindOf(node.mode);
    if (kind === 'symlink') {
      entries.set(path, { kind, mode: node.mode & 0o7777, target: node.target, uid: node.uid, gid: node.gid });
    } else if (kind === 'dir') {
      entries.set(path, { kind, mode: node.mode & 0o7777, uid: node.uid, gid: node.gid });
    } else {
      entries.set(path, {
        kind,
        mode: node.mode & 0o7777,
        size: node.size,
        uid: node.uid,
        gid: node.gid,
        sha256: args.hash ? sha256(node.read()) : null,
      });
    }
  }
  return { fs, entries };
}

function compare(reference, actual, { hash }) {
  let failures = 0;
  const fail = (message) => {
    failures += 1;
    if (failures <= 40) console.log(`  FAIL ${message}`);
  };
  for (const [path, want] of reference) {
    const got = actual.get(path);
    if (!got) {
      fail(`missing: ${path}`);
      continue;
    }
    if (got.kind !== want.kind) fail(`${path}: kind ${got.kind}, expected ${want.kind}`);
    // Ownership is in the image, not in an extracted tree: compare it only when
    // the expectation has it.
    if (want.uid !== undefined && (got.uid !== want.uid || got.gid !== want.gid)) {
      fail(`${path}: ${got.uid}:${got.gid}, expected ${want.uid}:${want.gid}`);
    }
    if (got.mode !== want.mode) fail(`${path}: mode 0${got.mode.toString(8)}, expected 0${want.mode.toString(8)}`);
    if (want.kind === 'file' && got.size !== want.size) fail(`${path}: size ${got.size}, expected ${want.size}`);
    if (want.kind === 'symlink' && got.target !== want.target) fail(`${path}: links to ${got.target}, expected ${want.target}`);
    if (hash && want.kind === 'file') {
      const expected = sha256(readFileSync(want.full));
      if (got.sha256 !== expected) fail(`${path}: contents differ`);
    }
  }
  for (const path of actual.keys()) if (path !== '' && !reference.has(path)) fail(`extra: ${path}`);
  console.log(`  compared ${reference.size} reference entries against ${actual.size} in the image`);
  return failures;
}

function extractTree(fs, dir) {
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
  for (const node of fs.walk()) {
    if (!node.path) continue;
    const target = join(dir, node.path);
    if (node.kind === 'dir') mkdirSync(target, { recursive: true });
    else if (node.kind === 'symlink') {
      mkdirSync(dirname(target), { recursive: true });
      writeFileSync(target, node.target);
    } else {
      mkdirSync(dirname(target), { recursive: true });
      writeFileSync(target, node.read());
    }
  }
  console.log(`  extracted to ${dir}`);
}

const args = parseArgs(process.argv);
console.log(`stock: ${args.stock}`);
const module = await loadModule(args);
const decompress = makeLzoDecompress(module);

const upt = new Uint8Array(readFileSync(args.stock));
const iso = readFirmwareIso(upt);
console.log(`  ISO: ${[...iso.files.keys()].join(', ')} (${iso.volume.sectors} sectors)`);

const system = iso.files.get('SYSTEM.UBI');
if (!system) throw new Error('the image has no SYSTEM.UBI');

const volume = new UbifsVolume(system.data, { lzoDecompress: decompress });
const summary = volume.summary();
console.log('  UBIFS:', {
  lebSize: volume.sb.lebSize,
  lebCount: summary.lebCount,
  file: summary.file,
  dir: summary.dir,
  symlink: summary.symlink,
  bytes: summary.bytes,
  badCrc: summary.badCrc,
  unsupported: summary.unsupported.length,
});
if (summary.badCrc !== 0) throw new Error(`${summary.badCrc} nodes with a bad CRC`);
if (summary.unsupported.length > 0) throw new Error(`unsupported nodes: ${summary.unsupported.slice(0, 5).join(', ')}`);

// Read: our tree against the reference extraction.
const { fs, entries } = readTree(volume, args);
if (args.extract) extractTree(fs, args.extract);

let failures = 0;
let reference = null;
try {
  reference = readReference(args.ref);
} catch (error) {
  console.log(`  reference tree ${args.ref} is not readable (${error.code ?? error.message}); skipping the comparison`);
}
// `build` edits the tree before packing, so the stock tree is not what the
// reference describes; only the rebuilt one is compared.
if (reference && args.mode !== 'build') failures += compare(reference, entries, { hash: args.hash });

/**
 * Packs the tree in memory, rewrites UPDATE.TXT when asked, reads the image back
 * and (optionally) writes the whole `.upt`.
 */
function packTree({ digests, out }) {
  mountTree(module, fs);
  const before = process.memoryUsage().heapUsed;
  const rc = runMkfs(module, firmwareMkfsArgs('/out.ubi'));
  if (rc !== 0) throw new Error(`mkfs.ubifs failed: ${rc}`);
  const image = module.FS.readFile('/out.ubi');
  console.log(`  mkfs.ubifs: ${image.length} bytes (${((process.memoryUsage().heapUsed - before) / 1e6).toFixed(0)} MB heap growth)`);

  let updateTxt = iso.files.get('UPDATE.TXT').data;
  if (digests) {
    const { updateDigests } = installModule;
    const rewritten = updateDigests(new TextDecoder().decode(updateTxt), {
      'SYSTEM.UBI': image,
      'UIMAGE.BIN': iso.files.get('UIMAGE.BIN').data,
    });
    for (const line of rewritten.lines) console.log(`  ${line}`);
    updateTxt = new TextEncoder().encode(rewritten.text);
  }

  // The new image is read back by the same reader: a tree that survives packing
  // and unpacking byte for byte is the check that matters here.
  const rebuilt = new UbifsVolume(image, { lzoDecompress: decompress });
  const rebuiltSummary = rebuilt.summary();
  console.log('  re-read:', {
    lebCount: rebuiltSummary.lebCount,
    file: rebuiltSummary.file,
    bytes: rebuiltSummary.bytes,
    badCrc: rebuiltSummary.badCrc,
  });
  if (rebuiltSummary.badCrc !== 0) throw new Error('the rebuilt image has nodes with a bad CRC');
  if (reference) failures += compare(reference, readTree(rebuilt, args).entries, { hash: args.hash });

  if (out) {
    const isoBytes = buildIso([
      { name: 'SYSTEM.UBI', data: image },
      { name: 'UIMAGE.BIN', data: iso.files.get('UIMAGE.BIN').data },
      { name: 'UBOOT.BIN', data: iso.files.get('UBOOT.BIN').data },
      { name: 'UPDATE.TXT', data: updateTxt },
      { name: 'VERSION.TXT', data: iso.files.get('VERSION.TXT').data },
      { name: '_GITIGNO', data: iso.files.get('_GITIGNO').data },
    ]);
    writeFileSync(out, isoBytes);
    console.log(`  wrote ${out} (${isoBytes.length} bytes)`);
  }
}

const installModule = await import('../../../web/lib/install.js');
const payloadModule = await import('../../../web/lib/payload.js');

/** The payload, out of the repository the way the pages workflow will pack it. */
function payloadFromRepo(dir) {
  const payloadEntries = [];
  const collect = (path, prefix) => {
    for (const name of readdirSync(path)) {
      const full = join(path, name);
      const st = lstatSync(full);
      const entryName = prefix ? `${prefix}/${name}` : name;
      const mtime = Math.floor(st.mtimeMs / 1000);
      if (st.isSymbolicLink()) {
        payloadEntries.push({ name: entryName, type: 'symlink', mode: 0o777, mtime, data: null, linkname: readlinkSync(full) });
      } else if (st.isDirectory()) {
        payloadEntries.push({ name: entryName, type: 'dir', mode: st.mode & 0o7777, mtime, data: null, linkname: null });
        collect(full, entryName);
      } else {
        payloadEntries.push({
          name: entryName,
          type: 'file',
          mode: st.mode & 0o7777,
          mtime,
          data: new Uint8Array(readFileSync(full)),
          linkname: null,
        });
      }
    }
  };
  collect(join(dir, 'assets/BLAZE'), 'BLAZE');
  collect(join(dir, 'assets/V3ANALOG'), 'V3ANALOG');
  const player = new Uint8Array(readFileSync(join(dir, 'sonix_player')));
  const launcher = new Uint8Array(readFileSync(join(dir, 'sonix_launch')));
  const meta = {
    format: 1,
    board: 'v3_analog',
    version: 'unknown',
    build_stamp: stampOf(join(dir, 'sonix_player')),
    player: 'sonix_player',
    launcher: 'sonix_launch',
    assets: ['BLAZE', 'V3ANALOG'],
  };
  payloadEntries.push({ name: 'sonix_player', type: 'file', mode: 0o775, mtime: 0, data: player, linkname: null });
  payloadEntries.push({ name: 'sonix_launch', type: 'file', mode: 0o775, mtime: 0, data: launcher, linkname: null });
  return payloadModule.payloadFromEntries(meta, payloadEntries);
}

/** `date -r <file> +%d%m%Y%H%M`, the stamp the player shows in About. */
function stampOf(path) {
  const date = new Date(lstatSync(path).mtimeMs);
  const pad = (value) => String(value).padStart(2, '0');
  return `${pad(date.getUTCDate())}${pad(date.getUTCMonth() + 1)}${date.getUTCFullYear()}${pad(date.getUTCHours())}${pad(date.getUTCMinutes())}`;
}

/** The version strings the reference image carries, so both builds write the same. */
function referenceIdentity(ref) {
  try {
    const info = JSON.parse(readFileSync(join(ref, 'usr/resource/sonix/components/system-info.json'), 'utf8'));
    return { osVersion: info.OS_version, buildStamp: info.build_version };
  } catch {
    return null;
  }
}

if (args.mode === 'write') packTree({ digests: false, out: args.out });

if (args.mode === 'build') {
  const payload = args.payloadArchive
    ? await payloadModule.loadPayload('payload.tar.gz', {
        fetch: async () => new Response(readFileSync(args.payloadArchive)),
      })
    : payloadFromRepo(args.payloadDir);
  // The archive carries its own identity, exactly as the page gets it. Without an
  // archive the identity is the reference image's, so both builds write the same
  // version strings.
  const identity = args.payloadArchive
    ? { buildStamp: payload.meta.build_stamp, osVersion: payload.meta.version }
    : referenceIdentity(args.ref);
  const buildStamp = identity?.buildStamp ?? payload.meta.build_stamp;
  const osVersion = identity?.osVersion ?? '1.3.0/webtest';
  console.log(`  payload: ${payload.summary().files} files, ${(payload.summary().bytes / 1048576).toFixed(1)} MB`);
  console.log(`  installing ${osVersion}, built ${buildStamp}`);
  for (const line of installModule.installTree(fs, { payload, buildStamp, osVersion })) console.log(`    ${line}`);
  for (const line of installModule.trimStock(fs).lines) console.log(`    ${line}`);
  packTree({ digests: true, out: args.out });
}

console.log(failures === 0 ? 'ok' : `${failures} failure(s)`);
process.exit(failures === 0 ? 0 : 1);
