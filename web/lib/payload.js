// The files the page needs that are not in the vendor's image.
//
// The stock firmware carries HiBy's player, not this port's. A build that runs in
// the user's browser therefore has to fetch three things: `sonix_player`,
// `sonix_launch`, and the two asset trees (`assets/BLAZE`, `assets/V3ANALOG` --
// the interface, the languages, the Bluetooth userland). They are this
// repository's own files and are published from it, so nothing here is the
// vendor's.
//
// The archive that carries all of it is a **release asset**
// (`firmware-payload-<board>.tar.gz`, attached by player-package.yml) and stays
// there. A browser cannot fetch a release asset -- the download redirects to
// release-assets.githubusercontent.com, which answers without
// Access-Control-Allow-Origin -- so the pages build takes each release's payload
// apart and serves the two halves from its own origin:
//
//   payload/<name>.player.tar.gz            sonix_player, sonix_launch, payload.json
//   payload/blobs/assets-<sha256>.tar.gz    assets/BLAZE + assets/V3ANALOG, one file
//                                           for every version whose assets did not change
//
// The player half is ~3 MB, the asset blob ~22 MB once. That is why the site does
// not grow by a full payload per release.
//
// `payload.json` (in the player half) carries the identity the image needs:
//
//   {"format":1, "board":"v3_analog", "version":"1.3.0/<tag>",
//    "build_stamp":"241120240140", "player":"sonix_player", "launcher":"sonix_launch",
//    "assets":["assets/BLAZE","assets/V3ANALOG"]}
//
// gzip is the browser's own (`DecompressionStream`), the tar layer is lib/tar.js.

import { untar } from './tar.js';
import { S_IFDIR } from './vfs.js';

export class PayloadError extends Error {}

export class Payload {
  /**
   * @param {object} meta contents of payload.json
   * @param {Array<{name: string, type: string, mode: number, mtime: number, data: Uint8Array|null, linkname: string|null}>} entries
   */
  constructor(meta, entries) {
    if (meta.format !== 1) throw new PayloadError(`payload format ${meta.format} is newer than this page`);
    if (meta.board !== 'v3_analog') throw new PayloadError(`payload is for ${meta.board}, this page builds v3_analog`);
    this.meta = meta;
    this.entries = entries;
  }

  file(name) {
    const entry = this.entries.find((candidate) => candidate.name === name && candidate.type === 'file');
    if (!entry) throw new PayloadError(`the payload has no ${name}`);
    return entry.data;
  }

  get player() {
    return this.file(this.meta.player);
  }

  get launcher() {
    return this.file(this.meta.launcher);
  }

  /**
   * Unpacks one asset tree into the Vfs, the way `tar cf - -C BASE . | tar xf - -C
   * TREE` does in the packer: directories merge, files overwrite, symlinks
   * replace, and every entry brings its mode and time.
   */
  overlay(vfs, root) {
    const prefix = `${root}/`;
    for (const entry of this.entries) {
      if (!entry.name.startsWith(prefix) || entry.type === 'dir') continue;
      const path = entry.name.slice(prefix.length);
      // The assets bring directories the stock image has not got (etc/ssl, for
      // one), and `tar x` would create them; so does this.
      const slash = path.lastIndexOf('/');
      if (slash > 0) vfs.mkdirTree(path.slice(0, slash));
      if (entry.type === 'file') vfs.writeFile(path, entry.data, entry.mode & 0o7777);
      else if (entry.type === 'symlink') {
        vfs.remove(path);
        vfs.symlink(entry.linkname, path);
      }
      const node = vfs.find(path);
      if (node) node.mtime = entry.mtime;
    }
    // Directories last: a mode is only meaningful on a directory that exists, and
    // tar's merge does not care in which order it walked them.
    for (const entry of this.entries) {
      if (!entry.name.startsWith(prefix)) continue;
      const path = entry.name.slice(prefix.length);
      const node = vfs.find(path);
      if (node) {
        node.mtime = entry.mtime;
        if ((entry.mode & S_IFDIR) !== 0 || entry.type === 'dir') node.mode = S_IFDIR | (entry.mode & 0o7777);
      }
    }
  }

  /** Every mode-carrying file, for the page's summary. */
  summary() {
    let files = 0;
    let bytes = 0;
    for (const entry of this.entries) {
      if (entry.type !== 'file') continue;
      files += 1;
      bytes += entry.data.length;
    }
    return { files, bytes, version: this.meta.version, stamp: this.meta.build_stamp };
  }
}

/** A payload out of entries already in memory (the Node harness builds one). */
export function payloadFromEntries(meta, entries) {
  return new Payload(meta, entries);
}

/**
 * The tar entries of one gzipped archive already in memory.
 *
 * @param {Uint8Array} gzipped
 * @returns {Promise<Array<object>>}
 */
export async function unpackArchive(gzipped) {
  const stream = new Blob([gzipped]).stream().pipeThrough(new DecompressionStream('gzip'));
  const reader = stream.getReader();
  const chunks = [];
  let length = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value);
    length += value.length;
  }
  const buffer = new Uint8Array(length);
  let offset = 0;
  for (const chunk of chunks) {
    buffer.set(chunk, offset);
    offset += chunk.length;
  }

  return untar(buffer);
}

/**
 * A complete payload in one archive: its entries plus the identity in
 * `payload.json`.
 *
 * @param {Uint8Array} gzipped
 * @returns {Promise<Payload>}
 */
export async function payloadFromArchive(gzipped) {
  const entries = await unpackArchive(gzipped);
  return payloadFromEntries(await metaOf(entries), entries);
}

async function metaOf(entries) {
  const metaEntry = entries.find((entry) => entry.name === 'payload.json' && entry.type === 'file');
  if (!metaEntry) throw new PayloadError('the payload has no payload.json');
  return JSON.parse(new TextDecoder().decode(metaEntry.data));
}

/**
 * One half of a version, fetched and checked.
 *
 * @param {{url: string, sha256?: string, size?: number}} part
 */
async function fetchArchive(part, options = {}) {
  const fetchImpl = options.fetch || fetch;
  const response = await fetchImpl(part.url);
  if (!response.ok) throw new PayloadError(`cannot fetch ${part.url}: HTTP ${response.status}`);
  const total = Number(response.headers.get('content-length') ?? part.size ?? 0);
  const reader = response.body.getReader();
  const chunks = [];
  let loaded = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value);
    loaded += value.length;
    options.onProgress?.({ loaded, total });
  }
  const gzipped = new Uint8Array(loaded);
  let offset = 0;
  for (const chunk of chunks) {
    gzipped.set(chunk, offset);
    offset += chunk.length;
  }
  if (part.sha256) {
    const digest = await sha256Hex(gzipped);
    if (digest !== part.sha256) {
      throw new PayloadError(`${part.url} has sha256 ${digest}, not ${part.sha256}`);
    }
  }
  return gzipped;
}

async function sha256Hex(bytes) {
  const digest = await crypto.subtle.digest('SHA-256', bytes);
  return [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, '0')).join('');
}

/**
 * Fetches and unpacks `url`, checking its digest when one is given.
 *
 * `DecompressionStream('gzip')` is in every browser this page supports, and in
 * Node 18+; there is no bundled inflater. The compressed bytes are kept while the
 * stream arrives because the digest covers the file as published, not the tree
 * inside it -- 26 MB of the page's memory for a check that a truncated or
 * substituted download cannot pass.
 */
export async function loadPayload(url, options = {}) {
  const gzipped = await fetchArchive({ url, sha256: options.sha256 }, options);
  return payloadFromArchive(gzipped);
}

/**
 * The payload of one version, whichever way `versions.json` describes it.
 *
 * Older entries carry one complete archive (`url` + `sha256`); the current layout
 * carries the two halves (`player`, `assets`), which is what keeps the site small.
 * Both are accepted, so a release published before the split keeps working.
 *
 * @param {object} entry an entry of versions.json
 */
export async function loadVersion(entry, options = {}) {
  if (entry.url) return loadPayload(entry.url, { ...options, sha256: entry.sha256 });

  const parts = [entry.player, entry.assets].filter(Boolean);
  if (parts.length === 0) throw new PayloadError(`version ${entry.label} names no payload`);
  const total = parts.reduce((sum, part) => sum + (part.size ?? 0), 0);
  let done = 0;
  const fetched = [];
  for (const part of parts) {
    const before = done;
    fetched.push(
      await fetchArchive(part, {
        ...options,
        onProgress: ({ loaded }) => {
          done = before + loaded;
          options.onProgress?.({ loaded: done, total });
        },
      }),
    );
  }

  // The halves are concatenated into one entry list: `Payload` finds files by
  // name and the asset overlay by prefix, so which archive a file came from stops
  // mattering -- that is what lets the asset blob be shared between versions. The
  // identity lives in the player half, the only one that carries payload.json.
  const entries = [];
  let meta = null;
  for (const archive of fetched) {
    const unpacked = await unpackArchive(archive);
    entries.push(...unpacked);
    meta = meta ?? (await metaOf(unpacked).catch(() => null));
  }
  if (!meta) throw new PayloadError('the version carries no payload.json');
  return new Payload(meta, entries);
}
