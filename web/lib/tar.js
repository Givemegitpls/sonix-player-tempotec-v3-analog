// tar, as much of it as a payload archive needs.
//
// The page fetches one `.tar.gz` carrying this port's files (the player, the
// launcher and the two asset trees -- `packs/` in the repository, packed by the
// pages workflow) and unpacks it in the worker. gzip comes from the platform
// (`DecompressionStream('gzip')`), so only the tar layer is here: ustar names,
// regular files, directories and symlinks, and the GNU long-name extension,
// because a rootfs path can pass 100 characters.
//
// Not implemented, deliberately: pax headers, sparse files, hard links, and
// writing. The payload is made by `tar czf` from directories this repository
// owns, and none of those exist in it.

const BLOCK = 512;

function readString(bytes, offset, length) {
  let end = offset;
  while (end < offset + length && bytes[end] !== 0) end += 1;
  return new TextDecoder().decode(bytes.subarray(offset, end));
}

function readNumber(bytes, offset, length) {
  // Octal with trailing spaces/NULs, as tar writes numbers; the GNU base-256
  // form (a high bit set) is not produced by `tar czf` for our sizes but is
  // cheap to accept.
  if (bytes[offset] & 0x80) {
    let value = 0;
    for (let i = offset + 1; i < offset + length; i += 1) value = value * 256 + bytes[i];
    return value;
  }
  const text = readString(bytes, offset, length).trim();
  return text === '' ? 0 : Number.parseInt(text, 8);
}

/**
 * @param {Uint8Array} bytes an uncompressed tar archive
 * @returns {Array<{name: string, type: 'file'|'dir'|'symlink'|'other', mode: number, size: number, mtime: number, data: Uint8Array|null, linkname: string|null}>}
 */
export function untar(bytes) {
  const entries = [];
  let offset = 0;
  let longName = null;
  let longLink = null;
  while (offset + BLOCK <= bytes.length) {
    const header = bytes.subarray(offset, offset + BLOCK);
    if (header.every((byte) => byte === 0)) break;
    const name = longName ?? readString(header, 0, 100);
    const mode = readNumber(header, 100, 8);
    const size = readNumber(header, 124, 12);
    const mtime = readNumber(header, 136, 12);
    const typeFlag = String.fromCharCode(header[156] || 0x30);
    const linkname = longLink ?? readString(header, 157, 100);
    const magic = readString(header, 257, 6);
    const prefix = magic.startsWith('ustar') ? readString(header, 345, 155) : '';
    // Archives made with `tar -cf - .` carry a leading `./`, which every tar strips
    // on extraction; names here are relative to the archive root.
    const path = (prefix ? `${prefix}/${name}` : name).replace(/^(\.\/)+/, '');

    offset += BLOCK;
    const data = bytes.subarray(offset, offset + size);
    offset += Math.ceil(size / BLOCK) * BLOCK;

    longName = null;
    longLink = null;
    if (typeFlag === 'L') {
      longName = readString(data, 0, data.length);
      continue;
    }
    if (typeFlag === 'K') {
      longLink = readString(data, 0, data.length);
      continue;
    }
    if (typeFlag === 'x' || typeFlag === 'g') {
      // pax headers are not produced for this payload; ignoring them is honest
      // about that rather than halfway-supporting them.
      continue;
    }

    const type =
      typeFlag === '5' ? 'dir' : typeFlag === '2' ? 'symlink' : typeFlag === '0' || typeFlag === '\0' ? 'file' : 'other';
    entries.push({ name: path, type, mode, size, mtime, data: type === 'file' ? data : null, linkname: type === 'symlink' ? linkname : null });
  }
  return entries;
}
