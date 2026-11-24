// ISO9660, the little of it the V3 Analog's firmware image uses.
//
// The update image (`v3_analog.upt`) is an ISO9660 filesystem with six files in
// one flat directory, no Rock Ridge, no Joliet -- genisoimage was called with
// `-l` and nothing else. So the reader is only a directory walk, and the writer
// only has to lay out a primary volume descriptor, one path table pair and one
// directory. No multi-extent, no interleaving, no level-3 names.
//
// Used both ways: by the browser page (which must not upload the vendor's file
// anywhere) and by `sonix-packer/tools/webtest`, which checks the result against
// what `sonix-packer/v3_analog_packer.sh` produces with 7z and genisoimage.
//
// Pure functions over Uint8Array: no Node API, no DOM.

export const SECTOR = 2048;

const PVD_SECTOR = 16;
const TERM_SECTOR = 17;
const PATH_TABLE_SECTOR = 18;
const ROOT_DIR_SECTOR = 20;
const FIRST_FILE_SECTOR = 24;

/** Both-endian (little then big) 32-bit field, as ISO9660 writes them. */
function putBoth32(buf, off, value) {
  buf[off] = value & 0xff;
  buf[off + 1] = (value >>> 8) & 0xff;
  buf[off + 2] = (value >>> 16) & 0xff;
  buf[off + 3] = (value >>> 24) & 0xff;
  buf[off + 4] = (value >>> 24) & 0xff;
  buf[off + 5] = (value >>> 16) & 0xff;
  buf[off + 6] = (value >>> 8) & 0xff;
  buf[off + 7] = value & 0xff;
}

function putBoth16(buf, off, value) {
  buf[off] = value & 0xff;
  buf[off + 1] = (value >>> 8) & 0xff;
  buf[off + 2] = (value >>> 8) & 0xff;
  buf[off + 3] = value & 0xff;
}

function getU32LE(buf, off) {
  return (buf[off] | (buf[off + 1] << 8) | (buf[off + 2] << 16) | (buf[off + 3] << 24)) >>> 0;
}

/**
 * The seven-byte recording date of ISO9660: years since 1900, month, day, hour,
 * minute, second, and a GMT offset in quarter hours. A fixed date keeps the
 * built image byte-stable for a given input, which is what makes the page's
 * output something a user can compare with someone else's.
 */
function putDate(buf, off, date) {
  buf[off] = Math.max(0, date.getUTCFullYear() - 1900) & 0xff;
  buf[off + 1] = date.getUTCMonth() + 1;
  buf[off + 2] = date.getUTCDate();
  buf[off + 3] = date.getUTCHours();
  buf[off + 4] = date.getUTCMinutes();
  buf[off + 5] = date.getUTCSeconds();
  buf[off + 6] = 0; // UTC
}

function readDate(buf, off) {
  return {
    year: buf[off] + 1900,
    month: buf[off + 1],
    day: buf[off + 2],
    hour: buf[off + 3],
    minute: buf[off + 4],
    second: buf[off + 5],
  };
}

function asciiSpace(buf, start, length, value) {
  buf.fill(0x20, start, start + length);
  const bytes = new TextEncoder().encode(value);
  buf.set(bytes.subarray(0, length), start);
}

function decodeAscii(buf, start, length) {
  return new TextDecoder('ascii').decode(buf.subarray(start, start + length)).trim();
}

/** Directory records, from one directory's data area. */
function* directoryRecords(buf, start, length) {
  let off = start;
  const end = start + length;
  while (off < end) {
    const len = buf[off];
    if (len === 0) {
      // Records do not cross sectors; the rest of this sector is padding.
      off = (Math.floor((off - start) / SECTOR) + 1) * SECTOR + start;
      continue;
    }
    if (off + len > end) return;
    const extent = getU32LE(buf, off + 2);
    const size = getU32LE(buf, off + 10);
    const flags = buf[off + 25];
    const nameLen = buf[off + 32];
    const raw = buf.subarray(off + 33, off + 33 + nameLen);
    yield { extent, size, flags, raw, date: readDate(buf, off + 18) };
    off += len;
  }
}

/** `NAME.EXT;1` -- what genisoimage writes for a file name without a version. */
function recordName(raw) {
  if (raw.length === 1 && (raw[0] === 0 || raw[0] === 1)) return raw[0] === 0 ? '.' : '..';
  const text = decodeAscii(raw, 0, raw.length);
  // The version separator: `;1`, and the `.` genisoimage leaves in front of it
  // when the name has no extension (`_GITIGNO.;1`).
  const semi = text.indexOf(';');
  const base = semi >= 0 ? text.slice(0, semi) : text;
  return base.endsWith('.') ? base.slice(0, -1) : base;
}

/**
 * Walks the whole filesystem and returns its entries with their contents.
 *
 * `data` is a subarray view of the image, not a copy: the image stays resident
 * once, and a page that only needs `SYSTEM.UBI` never materialises the other
 * five files.
 *
 * @returns {{volume: object, entries: Array<{path: string, name: string, size: number, isDir: boolean, date: object, data: Uint8Array|null}>}}
 */
export function parseIso(bytes) {
  if (bytes[PVD_SECTOR * SECTOR + 1] !== 0x43 /* C */) {
    throw new Error('not an ISO9660 image: no CD001 at sector 16');
  }
  const pvd = bytes.subarray(PVD_SECTOR * SECTOR, (PVD_SECTOR + 1) * SECTOR);
  const volume = {
    systemId: decodeAscii(pvd, 8, 32),
    volumeId: decodeAscii(pvd, 40, 32),
    sectors: getU32LE(pvd, 80),
    blockSize: pvd[128] | (pvd[129] << 8),
    fileStructureVersion: pvd[881],
  };

  const rootExtent = getU32LE(pvd, 156 + 2);
  const rootSize = getU32LE(pvd, 156 + 10);

  const entries = [];
  const walk = (extent, size, prefix) => {
    for (const record of directoryRecords(bytes, extent * SECTOR, size)) {
      const name = recordName(record.raw);
      if (name === '.' || name === '..') continue;
      const path = prefix ? `${prefix}/${name}` : name;
      const isDir = (record.flags & 2) !== 0;
      const data = isDir
        ? null
        : bytes.subarray(record.extent * SECTOR, record.extent * SECTOR + record.size);
      entries.push({ path, name, size: record.size, isDir, date: record.date, data });
      if (isDir) walk(record.extent, record.size, path);
    }
  };
  walk(rootExtent, rootSize, '');
  return { volume, entries };
}

/** The six files of a firmware image, by name, contents as views. */
export function readFirmwareIso(bytes) {
  const { volume, entries } = parseIso(bytes);
  const files = new Map();
  for (const entry of entries) if (!entry.isDir) files.set(entry.name, entry);
  return { volume, entries, files };
}

/**
 * Directory record size for a name of `nameLen` bytes: the 33-byte fixed part,
 * the name, and a padding byte when the total would be odd.
 */
function recordLength(nameLen) {
  const raw = 33 + nameLen;
  return raw % 2 === 0 ? raw : raw + 1;
}

function writeDirectoryRecord(buf, off, options) {
  const { name, extent, size, isDir, date } = options;
  const len = recordLength(name.length);
  buf.fill(0, off, off + len);
  buf[off] = len;
  buf[off + 1] = 0; // extended attribute length
  putBoth32(buf, off + 2, extent);
  putBoth32(buf, off + 10, size);
  putDate(buf, off + 18, date);
  buf[off + 25] = isDir ? 2 : 0;
  buf[off + 26] = 0; // file unit size
  buf[off + 27] = 0; // interleave gap
  putBoth16(buf, off + 28, 1); // volume sequence number
  buf[off + 32] = name.length;
  buf.set(name, off + 33);
  return len;
}

function encodeRecordName(name) {
  // Exactly what genisoimage -l produces for these names: upper case as given,
  // `.` plus `;1` after an extension, `.;1` after a bare name.
  const text = name.endsWith('.') ? `${name};1` : name.includes('.') ? `${name};1` : `${name}.;1`;
  return new TextEncoder().encode(text);
}

/**
 * Builds an ISO9660 image out of `[{name, data}]`, in the order given.
 *
 * The layout follows what genisoimage produces for this image: primary volume
 * descriptor at sector 16, terminator at 17, the two path tables at 18 and 19,
 * the root directory at 20, files from sector 24, each starting on a sector
 * boundary. Nothing in the update path depends on the positions (the recovery
 * kernel mounts the image), but keeping them the same as the vendor's file means
 * the shape of the image is one less thing that differs.
 */
export function buildIso(files, options = {}) {
  const date = options.date ?? new Date(Date.UTC(2024, 11, 24, 0, 0, 0));
  const volumeId = options.volumeId ?? '';

  const names = files.map((file) => encodeRecordName(file.name));
  // `.` and `..` are records of their own, and the directory's declared size is
  // the whole extent, rounded up to a sector -- what genisoimage writes, and what
  // a reader may rely on when it reads the directory as one block.
  const dirBytes =
    34 + 34 + names.reduce((total, name) => total + recordLength(name.length), 0);
  const rootDirSize = Math.ceil(dirBytes / SECTOR) * SECTOR;
  if (ROOT_DIR_SECTOR + rootDirSize / SECTOR > FIRST_FILE_SECTOR) {
    throw new Error(`${files.length} files do not fit in the root directory before sector ${FIRST_FILE_SECTOR}`);
  }

  // File extents, from sector 24 on.
  let sector = FIRST_FILE_SECTOR;
  const extents = files.map((file, index) => {
    const extent = sector;
    sector += Math.ceil(file.data.length / SECTOR);
    const name = names[index];
    void name;
    return { extent, size: file.data.length };
  });
  const totalSectors = sector;

  const out = new Uint8Array(totalSectors * SECTOR);

  // 16: primary volume descriptor.
  const pvd = out.subarray(PVD_SECTOR * SECTOR, (PVD_SECTOR + 1) * SECTOR);
  pvd[0] = 1;
  pvd.set(new TextEncoder().encode('CD001'), 1);
  pvd[6] = 1;
  asciiSpace(pvd, 8, 32, options.systemId ?? '');
  asciiSpace(pvd, 40, 32, volumeId);
  putBoth32(pvd, 80, totalSectors);
  putBoth16(pvd, 120, 1); // volume set size
  putBoth16(pvd, 124, 1); // volume sequence number
  putBoth16(pvd, 128, SECTOR);
  putBoth32(pvd, 132, 10); // path table size: one record for the root
  pvd[140] = PATH_TABLE_SECTOR & 0xff;
  pvd[141] = (PATH_TABLE_SECTOR >>> 8) & 0xff;
  pvd[148] = (PATH_TABLE_SECTOR + 1) >>> 24;
  pvd[149] = ((PATH_TABLE_SECTOR + 1) >>> 16) & 0xff;
  pvd[150] = ((PATH_TABLE_SECTOR + 1) >>> 8) & 0xff;
  pvd[151] = (PATH_TABLE_SECTOR + 1) & 0xff;
  writeDirectoryRecord(pvd, 156, { name: new Uint8Array([0]), extent: ROOT_DIR_SECTOR, size: rootDirSize, isDir: true, date });
  asciiSpace(pvd, 190, 128, '');
  asciiSpace(pvd, 318, 128, '');
  asciiSpace(pvd, 446, 128, '');
  asciiSpace(pvd, 574, 128, '');
  asciiSpace(pvd, 702, 37, '');
  asciiSpace(pvd, 739, 37, '');
  asciiSpace(pvd, 776, 37, '');
  pvd[881] = 1;
  // 882 is a reserved byte and must be zero: 7-Zip refuses the whole image when it
  // is not (found the hard way), and the spec says so anyway. 883..1394 is the
  // reserved field, which genisoimage fills with spaces.
  pvd[882] = 0;
  asciiSpace(pvd, 883, 512, '');

  // 17: volume descriptor set terminator.
  const term = out.subarray(TERM_SECTOR * SECTOR, (TERM_SECTOR + 1) * SECTOR);
  term[0] = 255;
  term.set(new TextEncoder().encode('CD001'), 1);
  term[6] = 1;

  // 18/19: the path table, little endian then big.
  const pathTable = (bigEndian) => {
    const buf = out.subarray(
      (PATH_TABLE_SECTOR + (bigEndian ? 1 : 0)) * SECTOR,
      (PATH_TABLE_SECTOR + (bigEndian ? 2 : 1)) * SECTOR,
    );
    buf[0] = 1; // length of the directory identifier
    buf[1] = 0; // extended attribute record length
    const put = (value, off) => {
      if (bigEndian) {
        buf[off] = (value >>> 24) & 0xff;
        buf[off + 1] = (value >>> 16) & 0xff;
        buf[off + 2] = (value >>> 8) & 0xff;
        buf[off + 3] = value & 0xff;
      } else {
        buf[off] = value & 0xff;
        buf[off + 1] = (value >>> 8) & 0xff;
        buf[off + 2] = (value >>> 16) & 0xff;
        buf[off + 3] = (value >>> 24) & 0xff;
      }
    };
    put(ROOT_DIR_SECTOR, 2);
    buf[6] = bigEndian ? 0 : 1; // parent directory number
    buf[7] = bigEndian ? 1 : 0;
    buf[8] = 0; // the root's identifier
  };
  pathTable(false);
  pathTable(true);

  // 20: the root directory, with `.`, `..` and one record per file.
  let off = ROOT_DIR_SECTOR * SECTOR;
  off += writeDirectoryRecord(out, off, { name: new Uint8Array([0]), extent: ROOT_DIR_SECTOR, size: rootDirSize, isDir: true, date });
  off += writeDirectoryRecord(out, off, { name: new Uint8Array([1]), extent: ROOT_DIR_SECTOR, size: rootDirSize, isDir: true, date });
  files.forEach((file, index) => {
    const name = names[index];
    const recordLen = recordLength(name.length);
    const inSector = (off - ROOT_DIR_SECTOR * SECTOR) % SECTOR;
    if (inSector + recordLen > SECTOR) off += SECTOR - inSector;
    off += writeDirectoryRecord(out, off, {
      name,
      extent: extents[index].extent,
      size: extents[index].size,
      isDir: false,
      date,
    });
  });

  // The files themselves.
  files.forEach((file, index) => {
    out.set(file.data, extents[index].extent * SECTOR);
  });

  return out;
}
