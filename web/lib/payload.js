// The files the page needs that are not in the vendor's image.
//
// The stock firmware carries HiBy's player, not this port's. A build that runs in
// the user's browser therefore has to fetch three things: `sonix_player`,
// `sonix_launch`, and the two asset trees (`assets/BLAZE`, `assets/V3ANALOG` --
// the interface, the languages, the Bluetooth userland). They are this
// repository's own files and are published from it, so nothing here is the
// vendor's.
//
// One archive, `payload.tar.gz`, made by `.github/workflows/pages.yml` and served
// from the same Pages origin as the page, with `payload.json` inside carrying the
// identity the image needs:
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
 * Fetches and unpacks `url`.
 *
 * `DecompressionStream('gzip')` is in every browser this page supports, and in
 * Node 18+; there is no bundled inflater.
 */
export async function loadPayload(url, options = {}) {
  const fetchImpl = options.fetch || fetch;
  const response = await fetchImpl(url);
  if (!response.ok) throw new PayloadError(`cannot fetch ${url}: HTTP ${response.status}`);
  const total = Number(response.headers.get('content-length') ?? 0);
  const reader = response.body.pipeThrough(new DecompressionStream('gzip')).getReader();
  const chunks = [];
  let loaded = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value);
    loaded += value.length;
    options.onProgress?.({ loaded, total });
  }
  const buffer = new Uint8Array(loaded);
  let offset = 0;
  for (const chunk of chunks) {
    buffer.set(chunk, offset);
    offset += chunk.length;
  }

  const entries = untar(buffer);
  const metaEntry = entries.find((entry) => entry.name === 'payload.json' && entry.type === 'file');
  if (!metaEntry) throw new PayloadError('the payload has no payload.json');
  const meta = JSON.parse(new TextDecoder().decode(metaEntry.data));
  return new Payload(meta, entries);
}
