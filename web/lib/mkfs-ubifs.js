// The bridge between the rootfs tree and `mkfs.ubifs` in WebAssembly.
//
// mkfs.ubifs is the C program from mtd-utils, compiled for wasm32 by
// sonix-packer/tools/wasm/build.sh. This module lends it the tree and takes the
// image back:
//
//   const module = await createMkfsUbifs({ wasmBinary, locateFile });
//   mountTree(module, vfs);                       // /tree, read-only, lazy
//   const rc = runMkfs(module, firmwareMkfsArgs('/out.ubi'));
//   const image = module.FS.readFile('/out.ubi');
//
// Why a custom mount instead of copying files into the module's MEMFS: the stock
// rootfs is 69 MB of files that decompress out of 47 MB of LZO nodes, and the
// image is another 47 MB. A copy would put the whole tree on the heap twice for
// no reason -- the mount serves `read()` straight out of the lazily decompressed
// nodes (web/lib/vfs.js). It is also what keeps the modes exact: every node in the
// mount reports the mode the VFS holds, not MEMFS's default.
//
// The same module exports the LZO decompressor, so `makeLzoDecompress` is what
// the reader uses on the stock image: one implementation of LZO in the page, the
// one that wrote the nodes.

import { S_IFDIR, S_IFMT } from './vfs.js';

const MOUNT = '/tree';

function makeNodeOps(FS) {
  return {
    // stat() is the only thing mkfs.ubifs asks of a directory besides readdir.
    getattr(node) {
      const vfsNode = node.vfsNode;
      const kind = vfsNode.mode & S_IFMT;
      const size =
        kind === S_IFDIR ? 4096 : kind === 0o120000 ? new TextEncoder().encode(vfsNode.target ?? '').length : vfsNode.size;
      return {
        dev: 1,
        ino: node.id,
        mode: vfsNode.mode,
        nlink: kind === S_IFDIR ? 2 : 1,
        uid: vfsNode.uid,
        gid: vfsNode.gid,
        rdev: 0,
        size,
        atime: new Date(vfsNode.atime * 1000),
        mtime: new Date(vfsNode.mtime * 1000),
        ctime: new Date(vfsNode.ctime * 1000),
        blksize: 4096,
        blocks: Math.ceil(size / 4096),
      };
    },
    // mkfs.ubifs clears append-only/immutable flags through an ioctl (stubbed to
    // ENOTTY) and never writes to the source; this exists so that a stray chmod
    // from a future call site changes nothing.
    setattr(node, attr) {
      if (attr.timestamp !== undefined) node.vfsNode.mtime = Math.floor(attr.timestamp / 1000);
    },
    lookup(parent, name) {
      const child = parent.vfsNode.children?.get(name);
      if (!child) throw new FS.ErrnoError(44); // ENOENT
      return makeFsNode(FS, parent, name, child);
    },
    readdir(node) {
      return ['.', '..', ...(node.vfsNode.children?.keys() ?? [])];
    },
    readlink(node) {
      if ((node.vfsNode.mode & S_IFMT) !== 0o120000) throw new FS.ErrnoError(28); // EINVAL
      return node.vfsNode.target;
    },
  };
}

function makeStreamOps(FS) {
  return {
    open() {},
    close() {},
    read(stream, buffer, offset, length, position) {
      const data = stream.node.vfsNode.read();
      if (position >= data.length) return 0;
      const size = Math.min(data.length - position, length);
      buffer.set(data.subarray(position, position + size), offset);
      return size;
    },
    llseek(stream, offset, whence) {
      let position = offset;
      if (whence === 1) position += stream.position;
      else if (whence === 2) position += stream.node.vfsNode.size;
      if (position < 0) throw new FS.ErrnoError(28);
      return position;
    },
  };
}

/**
 * One FS node per VFS node, created on first lookup and reused afterwards: the
 * module's name table hashes by (parent, name), and a second node object for the
 * same entry would show up as a duplicate in `readdir` and as a different inode
 * number in `stat`.
 */
function makeFsNode(FS, parent, name, vfsNode) {
  if (vfsNode.fsNode) return vfsNode.fsNode;
  const node = FS.createNode(parent, name, vfsNode.mode, 0);
  node.vfsNode = vfsNode;
  vfsNode.fsNode = node;
  const ops = makeNodeOps(FS);
  node.node_ops = ops;
  node.stream_ops =
    (vfsNode.mode & S_IFMT) === S_IFDIR
      ? // A directory is a stream of names: getdents moves through it by seeking
        // from the current position, so it needs a seek even though it has no
        // read of its own.
        {
          llseek(stream, offset, whence) {
            const position = whence === 1 ? stream.position + offset : offset;
            if (position < 0) throw new FS.ErrnoError(28);
            return position;
          },
        }
      : makeStreamOps(FS);
  return node;
}

/**
 * Mounts the tree at /tree, read-only.
 *
 * @param {object} module the emscripten module
 * @param {import('./vfs.js').Vfs} vfs
 */
export function mountTree(module, vfs) {
  const { FS } = module;
  const ops = makeNodeOps(FS);
  // emscripten's FS.readlink() returns the target resolved against the link's
  // directory, which is what a shell wants. mkfs.ubifs stores what the kernel
  // reports -- the target as written ("busybox", not "/tree/bin/busybox") -- so
  // the readlink syscall has to hand it the raw string. lookupPath()'s own symlink
  // traversal resolves the result itself, so this is safe for the rest of the FS.
  FS.readlink = (path) => {
    const { node } = FS.lookupPath(path);
    if (!node.node_ops.readlink) throw new FS.ErrnoError(28); // EINVAL
    return node.node_ops.readlink(node);
  };
  const root = FS.createNode(null, '/', vfs.root.mode, 0);
  root.vfsNode = vfs.root;
  vfs.root.fsNode = root;
  root.node_ops = ops;
  root.stream_ops = {
    llseek(stream, offset, whence) {
      const position = whence === 1 ? stream.position + offset : offset;
      if (position < 0) throw new FS.ErrnoError(28);
      return position;
    },
  };
  const treeFs = { mount: () => root };
  FS.mkdir(MOUNT);
  FS.mount(treeFs, {}, MOUNT);
  return root;
}

/** Calls mkfs.ubifs' entry point (see -Dmain=mkfs_ubifs_main in tools/wasm). */
export function runMkfs(module, args) {
  // The pointers must be allocated one at a time: malloc may grow the heap, and
  // the HEAPU32 view is replaced when it does.
  const pointer = module._malloc((args.length + 1) * 4);
  for (let i = 0; i < args.length; i += 1) {
    const length = module.lengthBytesUTF8(args[i]) + 1;
    const text = module._malloc(length);
    module.stringToUTF8(args[i], text, length);
    module.HEAPU32[(pointer >> 2) + i] = text;
  }
  module.HEAPU32[(pointer >> 2) + args.length] = 0;
  const rc = module._mkfs_ubifs_main(args.length, pointer);
  for (let i = 0; i < args.length; i += 1) module._free(module.HEAPU32[(pointer >> 2) + i]);
  module._free(pointer);
  return rc;
}

/**
 * The arguments `sonix-packer/v3_analog_packer.sh` gives mkfs.ubifs, and the
 * geometry of this device's rootfs volume: 480 LEBs of 0x1F000 bytes, 2 KB
 * minimum I/O, LZO. `-c` is the volume's size, so a tree that does not fit fails
 * the build the same way it fails the packer.
 */
export function firmwareMkfsArgs(output = '/out.ubi') {
  return ['mkfs.ubifs', '-x', 'lzo', '-e', '0x1F000', '-c', '480', '-m', '0x800', '-d', MOUNT, '-o', output];
}

/**
 * A decompressor for the stock image's data nodes, out of the same module.
 *
 * LZO's entry point takes the compressed bytes, their length, an output buffer
 * and an in/out length; the wrapper copies through the heap and returns a view of
 * the result. 16 485 of the vendor rootfs's 18 060 data nodes are LZO.
 */
export function makeLzoDecompress(module) {
  const rc = module._wasm_lzo_init();
  if (rc !== 0) throw new Error(`lzo_init failed: ${rc}`);
  return (payload, outputSize) => {
    const inPtr = module._malloc(payload.length);
    const outPtr = module._malloc(outputSize);
    const outLenPtr = module._malloc(4);
    try {
      module.HEAPU8.set(payload, inPtr);
      module.HEAPU32[outLenPtr >> 2] = outputSize;
      const result = module._wasm_lzo1x_decompress_safe(inPtr, payload.length, outPtr, outLenPtr);
      if (result !== 0) throw new Error(`lzo1x_decompress_safe failed: ${result}`);
      const produced = module.HEAPU32[outLenPtr >> 2];
      return module.HEAPU8.slice(outPtr, outPtr + produced);
    } finally {
      module._free(inPtr);
      module._free(outPtr);
      module._free(outLenPtr);
    }
  };
}

export { MOUNT };
