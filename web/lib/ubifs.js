// UBIFS, read side: what a V3 Analog firmware image contains.
//
// `SYSTEM.UBI` inside the vendor's `.upt` is not a UBI container -- there are no
// EC/VID headers in it, it is a bare UBIFS image, which is what `mkfs.ubifs`
// writes with `-o`. (ubi_reader reads both, which is why the packer calls it.)
// So the reader below needs no eraseblock volume layer at all: the file is a
// sequence of LEBs of `sb.leb_size` bytes, each carrying nodes that start at
// 8-byte boundaries and are otherwise separated by 0xFF fill.
//
// The tree is rebuilt by a linear pass over every LEB, not by walking the index
// B-tree: a node that was superseded stays in the image and carries a smaller
// `sqnum`, so keeping the highest `sqnum` per (inode), per (inode, block) and
// per (directory entry) yields the committed filesystem. That is far less code
// than a B-tree descent in C's terms, and it is checked against a tree that
// ubi_reader extracted (`sonix-packer/tools/webtest`).
//
// What is deliberately absent: on-disk index/LPT/journal interpretation, xattr
// nodes, and write support. The port's own filesystems have no device nodes, no
// FIFOs, no extended attributes and no hard-linked files, which the same test
// verifies, so their absence here is checked rather than assumed.
//
// Pure functions over Uint8Array: no Node API, no DOM.

const NODE_MAGIC = 0x06101831;
const UBIFS_BLOCK_SIZE = 4096;

export const NODE_TYPES = [
  'ino', 'data', 'dent', 'xent', 'trun', 'pad', 'sb', 'mst',
  'ref', 'idx', 'cs', 'orph', 'auth', 'sig',
];
const T_INO = 0;
const T_DATA = 1;
const T_DENT = 2;
const T_PAD = 5;
const T_SB = 6;
const T_IDX = 9;

export const UBI_INO_TYPE = { REG: 0, DIR: 1, LNK: 2, BLK: 3, CHR: 4, FIFO: 5, SOCK: 6 };

/** CRC32, as zlib computes it. */
const crcTable = (() => {
  const table = new Uint32Array(256);
  for (let i = 0; i < 256; i += 1) {
    let value = i;
    for (let bit = 0; bit < 8; bit += 1) value = value & 1 ? 0xedb88320 ^ (value >>> 1) : value >>> 1;
    table[i] = value >>> 0;
  }
  return table;
})();

export function crc32(bytes, start = 0, end = bytes.length) {
  let crc = 0xffffffff;
  for (let i = start; i < end; i += 1) crc = crcTable[(crc ^ bytes[i]) & 0xff] ^ (crc >>> 8);
  return crc >>> 0;
}

/**
 * UBIFS stores the CRC over everything after the magic and CRC fields, seeded
 * with 0xFFFFFFFF and not finalised. `crc32` above is that same raw register
 * value (`zlib.crc32(...) ^ 0xFFFFFFFF`), so the node's field is compared with it
 * directly -- verified against the vendor image.
 */
export function nodeCrc(bytes, start, len) {
  return crc32(bytes, start + 8, start + len);
}

function u16(bytes, off) {
  return bytes[off] | (bytes[off + 1] << 8);
}

function u32(bytes, off) {
  return (bytes[off] | (bytes[off + 1] << 8) | (bytes[off + 2] << 16) | (bytes[off + 3] << 24)) >>> 0;
}

function u64(bytes, off) {
  const low = u32(bytes, off);
  const high = u32(bytes, off + 4);
  return high * 0x100000000 + low;
}

export function parseSuperblock(bytes) {
  if (u32(bytes, 0) !== NODE_MAGIC) throw new Error('not a UBIFS image: no node magic at offset 0');
  if (bytes[20] !== T_SB) throw new Error(`first node is ${NODE_NAMES[bytes[20]] ?? bytes[20]}, not a superblock`);
  const sb = {
    sqnum: u64(bytes, 8),
    len: u32(bytes, 16),
    keyHash: bytes[26],
    keyFormat: bytes[27],
    flags: u32(bytes, 28),
    minIoSize: u32(bytes, 32),
    lebSize: u32(bytes, 36),
    lebCount: u32(bytes, 40),
    maxLebCount: u32(bytes, 44),
    maxBudBytes: u64(bytes, 48),
    logLebs: u32(bytes, 56),
    lptLebs: u32(bytes, 60),
    orphLebs: u32(bytes, 64),
    jheadCnt: u32(bytes, 68),
    fanout: u32(bytes, 72),
    lsaveCnt: u32(bytes, 76),
    fmtVersion: u32(bytes, 80),
    defaultCompr: u16(bytes, 84),
  };
  if (sb.keyFormat !== 0) {
    throw new Error(`UBIFS key format ${sb.keyFormat}; this reader implements only R5 (0)`);
  }
  return sb;
}

const NODE_NAMES = Object.fromEntries(NODE_TYPES.map((name, index) => [index, name]));

/**
 * All valid nodes of the image, in image order.
 *
 * A node whose CRC does not match is skipped and counted: an image built by this
 * port has none, and a count above zero means the input is not what it claims.
 */
export function scanNodes(bytes, sb) {
  const nodes = [];
  const lebSize = sb.lebSize;
  const lebCount = Math.min(sb.lebCount, Math.floor(bytes.length / lebSize));
  let badCrc = 0;
  const counts = new Map();
  for (let leb = 0; leb < lebCount; leb += 1) {
    const lebStart = leb * lebSize;
    let off = 0;
    while (off + 24 <= lebSize) {
      if (u32(bytes, lebStart + off) !== NODE_MAGIC) {
        off += 8;
        continue;
      }
      const len = u32(bytes, lebStart + off + 16);
      const type = bytes[lebStart + off + 20];
      if (len < 24 || len > lebSize - off || type >= NODE_TYPES.length) {
        off += 8;
        continue;
      }
      if (nodeCrc(bytes, lebStart + off, len) !== u32(bytes, lebStart + off + 4)) {
        badCrc += 1;
        off += (len + 7) & ~7;
        continue;
      }
      nodes.push({ leb, offset: off, start: lebStart + off, len, type, sqnum: u64(bytes, lebStart + off + 8) });
      counts.set(type, (counts.get(type) ?? 0) + 1);
      off += (len + 7) & ~7;
    }
  }
  return { nodes, badCrc, counts, lebCount };
}

function parseInode(bytes, node) {
  const base = node.start + 24 + 16;
  const inode = {
    inum: u32(bytes, node.start + 24),
    sqnum: node.sqnum,
    size: u64(bytes, base + 8),
    atime: u64(bytes, base + 16),
    ctime: u64(bytes, base + 24),
    mtime: u64(bytes, base + 32),
    atimeNsec: u32(bytes, base + 40),
    ctimeNsec: u32(bytes, base + 44),
    mtimeNsec: u32(bytes, base + 48),
    nlink: u32(bytes, base + 52),
    uid: u32(bytes, base + 56),
    gid: u32(bytes, base + 60),
    mode: u32(bytes, base + 64),
    flags: u32(bytes, base + 68),
    dataLen: u32(bytes, base + 72),
    xattrCount: u32(bytes, base + 76),
    xattrNames: u32(bytes, base + 88),
    comprType: u16(bytes, base + 92),
    inline: null,
  };
  if (inode.dataLen > 0 && node.len >= base - node.start + 120 + inode.dataLen) {
    // Small file stored inside the inode node itself.
    inode.inline = bytes.subarray(base + 120, base + 120 + inode.dataLen);
  }
  return inode;
}

function parseData(bytes, node) {
  const base = node.start + 24;
  const inum = u32(bytes, base);
  const block = u32(bytes, base + 4) & 0x1fffffff;
  return {
    inum,
    block,
    sqnum: node.sqnum,
    size: u32(bytes, base + 16),
    comprType: u16(bytes, base + 20),
    payload: bytes.subarray(base + 24, node.start + node.len),
  };
}

function parseDent(bytes, node) {
  const base = node.start + 24;
  const parent = u32(bytes, base);
  // Fixed part of a dent node: ch(24) key(16) inum(8) pad(2) nlen(2) type(1)
  // pad(3) = 56, then the name and a NUL. Measured on the vendor image: nine
  // bytes of name ("aux-cache") in a 66-byte node is 56 + 9 + 1.
  const DENT_NAME_OFFSET = 56;
  const nameLen = u16(bytes, base + 26);
  const type = bytes[base + 28];
  let nameStart = node.start + DENT_NAME_OFFSET;
  if (nameStart + nameLen > node.start + node.len) nameStart = node.start + node.len - nameLen - 1;
  if (nameLen === 0 || nameStart < node.start + 24) return null;
  return {
    parent,
    sqnum: node.sqnum,
    inum: u64(bytes, base + 16),
    type,
    name: new TextDecoder().decode(bytes.subarray(nameStart, nameStart + nameLen)),
  };
}

/**
 * A UBIFS image as a filesystem: inodes, directory entries, and file contents on
 * demand.
 *
 * `lzoDecompress(payload, outputSize)` is injected -- the browser passes the
 * function a WASM module exports, so the decompressor is compiled once (it is the
 * same `lzo1x_decompress_safe` the packer's `mkfs.ubifs` uses to write) instead
 * of being reimplemented in JavaScript.
 */
export class UbifsVolume {
  /**
   * @param {Uint8Array} bytes
   * @param {{lzoDecompress?: (payload: Uint8Array, outputSize: number) => Uint8Array, checkCrc?: boolean}} [options]
   */
  constructor(bytes, options = {}) {
    this.bytes = bytes;
    this.lzoDecompress = options.lzoDecompress ?? null;
    this.sb = parseSuperblock(bytes);
    const scanned = scanNodes(bytes, this.sb);
    this.badCrc = scanned.badCrc;
    this.lebCount = scanned.lebCount;
    this.nodeCounts = scanned.counts;

    /** @type {Map<number, ReturnType<typeof parseInode>>} */
    this.inodes = new Map();
    /** @type {Map<number, Map<number, ReturnType<typeof parseData>>>} */
    this.dataNodes = new Map();
    /** @type {Map<number, Map<string, ReturnType<typeof parseDent>>>} */
    this.dentries = new Map();
    /** @type {Map<number, Map<string, {value: Uint8Array}>>} */
    this.xattrs = new Map();
    const problems = [];

    for (const node of scanned.nodes) {
      switch (node.type) {
        case T_INO: {
          const inode = parseInode(bytes, node);
          const seen = this.inodes.get(inode.inum);
          if (!seen || seen.sqnum < inode.sqnum) this.inodes.set(inode.inum, inode);
          break;
        }
        case T_DATA: {
          const data = parseData(bytes, node);
          let blocks = this.dataNodes.get(data.inum);
          if (!blocks) this.dataNodes.set(data.inum, (blocks = new Map()));
          const seen = blocks.get(data.block);
          if (!seen || seen.sqnum < data.sqnum) blocks.set(data.block, data);
          break;
        }
        case T_DENT: {
          const dent = parseDent(bytes, node);
          if (!dent) break;
          let entries = this.dentries.get(dent.parent);
          if (!entries) this.dentries.set(dent.parent, (entries = new Map()));
          const seen = entries.get(dent.name);
          if (!seen || seen.sqnum < dent.sqnum) entries.set(dent.name, dent);
          break;
        }
        case T_IDX:
        case T_PAD:
        case T_SB:
        case 4: // trun: truncation, already reflected in the inode's size
        case 7: // mst: master node, the B-tree roots the linear pass does not need
        case 8: // ref: index references
        case 10: // cs: commit start
        case 11: // orph: orphan area
          break;
        default:
          problems.push(`node type ${NODE_NAMES[node.type] ?? node.type} at LEB ${node.leb}+${node.offset}`);
      }
    }
    this.unsupported = problems;
  }

  get rootInode() {
    return 1;
  }

  inode(inum) {
    const inode = this.inodes.get(inum);
    if (!inode) throw new Error(`inode ${inum} is not in the image`);
    return inode;
  }

  /** Directory entries of one directory, name -> dent, in name order. */
  directory(inum) {
    const entries = this.dentries.get(inum) ?? new Map();
    return new Map([...entries].sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0)));
  }

  /** Raw contents of a regular file or a symlink target. */
  readFile(inum) {
    const inode = this.inode(inum);
    const out = new Uint8Array(inode.size);
    if (inode.inline && inode.inline.length >= inode.size) {
      out.set(inode.inline.subarray(0, inode.size));
      return out;
    }
    if (inode.inline) out.set(inode.inline);
    const blocks = this.dataNodes.get(inum);
    for (const [block, data] of blocks ?? new Map()) {
      const offset = block * UBIFS_BLOCK_SIZE;
      if (offset >= out.length) continue;
      const size = Math.min(data.size, out.length - offset);
      if (data.comprType === 0) {
        out.set(data.payload.subarray(0, size), offset);
        continue;
      }
      if (!this.lzoDecompress) {
        throw new Error(
          `inode ${inum} block ${block} is LZO-compressed and no decompressor was supplied`,
        );
      }
      const block_ = this.lzoDecompress(data.payload, data.size);
      out.set(block_.subarray(0, size), offset);
    }
    return out;
  }

  readlink(inum) {
    return new TextDecoder().decode(this.readFile(inum));
  }

  /** Every path in the filesystem, depth first, directories before contents. */
  *walk(startInum = 1) {
    const stack = [{ inum: startInum, path: '' }];
    while (stack.length > 0) {
      const { inum, path } = stack.pop();
      const inode = this.inode(inum);
      yield { path, inum, inode };
      if ((inode.mode & 0o170000) !== 0o040000) continue;
      const children = [...this.directory(inum)].reverse();
      for (const [name, dent] of children) {
        stack.push({ inum: dent.inum, path: path ? `${path}/${name}` : name });
      }
    }
  }

  summary() {
    const kinds = { file: 0, dir: 0, symlink: 0, other: 0 };
    let bytes = 0;
    let links = 0;
    let nodes = 0;
    for (const { inode } of this.walk()) {
      nodes += 1;
      const kind = inode.mode & 0o170000;
      if (kind === 0o100000) {
        kinds.file += 1;
        bytes += inode.size;
      } else if (kind === 0o040000) {
        kinds.dir += 1;
      } else if (kind === 0o120000) {
        kinds.symlink += 1;
      } else {
        kinds.other += 1;
      }
      links += inode.nlink > 1 && kind === 0o100000 ? 1 : 0;
    }
    return {
      ...kinds,
      bytes,
      hardLinks: links,
      reachable: nodes,
      inodes: this.inodes.size,
      badCrc: this.badCrc,
      unsupported: this.unsupported,
      lebCount: this.lebCount,
      nodeCounts: Object.fromEntries([...this.nodeCounts].map(([type, count]) => [NODE_NAMES[type] ?? type, count])),
    };
  }
}

export { UBIFS_BLOCK_SIZE, NODE_MAGIC };
