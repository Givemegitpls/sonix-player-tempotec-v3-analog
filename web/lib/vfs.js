// The rootfs tree the firmware page edits, as plain objects.
//
// It is built from the vendor's image (either the stock one, or one this page
// built before), edited with the same operations `tools/install_tree.sh` and
// `tools/trim_stock.py` perform on a real tree, and handed to `mkfs.ubifs` in
// wasm as a read-only mount -- never copied into the module's own filesystem,
// because the stock rootfs is 69 MB of files and 300 MB of directories, and that
// copy would be the largest allocation on the page.
//
// File contents load lazily: a node keeps a `reader` closure that pulls the
// compressed data nodes out of the source volume only when mkfs.ubifs asks for
// that file, so the trim step can delete a 7 MB library without ever
// decompressing it.
//
// No Node API and no DOM: the same code runs in the page, in its worker, and in
// sonix-packer/tools/webtest under Node.

export const S_IFMT = 0o170000;
export const S_IFREG = 0o100000;
export const S_IFDIR = 0o040000;
export const S_IFLNK = 0o120000;

export function kindOf(mode) {
  switch (mode & S_IFMT) {
    case S_IFREG:
      return 'file';
    case S_IFDIR:
      return 'dir';
    case S_IFLNK:
      return 'symlink';
    default:
      return `other(0${(mode & S_IFMT).toString(8)})`;
  }
}

/** One filesystem node, as much of one as a firmware image needs. */
export class Node {
  constructor({ name, mode, uid = 0, gid = 0, mtime = 0, atime = 0, ctime = 0 }) {
    this.name = name;
    this.mode = mode;
    this.uid = uid;
    this.gid = gid;
    this.mtime = mtime;
    this.atime = atime ?? mtime;
    this.ctime = ctime ?? mtime;
    this.parent = null;
    /** @type {Map<string, Node>|null} */
    this.children = (mode & S_IFMT) === S_IFDIR ? new Map() : null;
    /** Symlink target. */
    this.target = null;
    /** @type {Uint8Array|null} loaded contents of a regular file */
    this.content = null;
    /** @type {(() => Uint8Array)|null} where the contents come from */
    this.reader = null;
    this.size = 0;
  }

  get kind() {
    return kindOf(this.mode);
  }

  get isDir() {
    return this.children !== null;
  }

  get path() {
    if (!this.parent) return '';
    const parent = this.parent.path;
    return parent ? `${parent}/${this.name}` : this.name;
  }

  /** Contents, decompressed on first use and kept for the rest of the build. */
  read() {
    if (this.content) return this.content;
    if (this.kind === 'symlink') {
      this.content = new TextEncoder().encode(this.target ?? '');
    } else if (this.reader) {
      this.content = this.reader();
    } else {
      this.content = new Uint8Array(this.size);
    }
    return this.content;
  }

  setContent(bytes) {
    this.content = bytes;
    this.size = bytes.length;
    this.reader = null;
  }
}

export class VfsError extends Error {
  constructor(code, path) {
    super(`${code}: ${path}`);
    this.code = code;
    this.path = path;
  }
}

/**
 * A tree: lookups, mutations, and a walk. Paths are absolute and always start at
 * `/`; no `.`/`..`, no trailing slashes.
 */
export class Vfs {
  constructor(root) {
    this.root = root;
  }

  /** Builds the tree of a UBIFS volume, contents still compressed. */
  static fromUbifs(volume) {
    const nodes = new Map();
    const root = new Node({ name: '', mode: volume.inode(1).mode, mtime: volume.inode(1).mtime });
    nodes.set(1, root);
    const build = (inum, node, parent) => {
      const inode = volume.inode(inum);
      node.parent = parent;
      node.mode = inode.mode;
      node.uid = inode.uid;
      node.gid = inode.gid;
      node.mtime = inode.mtime;
      node.atime = inode.atime;
      node.ctime = inode.ctime;
      if (kindOf(inode.mode) === 'symlink') {
        node.target = volume.readlink(inum);
        node.size = inode.size;
      } else if (kindOf(inode.mode) === 'file') {
        node.size = inode.size;
        node.reader = () => volume.readFile(inum);
      }
      nodes.set(inum, node);
      if (node.children) {
        for (const [name, dent] of volume.directory(inum)) {
          const child = new Node({ name, mode: volume.inode(dent.inum).mode });
          node.children.set(name, child);
          build(dent.inum, child, node);
        }
      }
    };
    build(1, root, null);
    return new Vfs(root);
  }

  /** The node at `path`, or undefined. */
  find(path) {
    const parts = path.split('/').filter(Boolean);
    let node = this.root;
    for (const part of parts) {
      if (!node.children) return undefined;
      node = node.children.get(part);
      if (!node) return undefined;
    }
    return node;
  }

  require(path) {
    const node = this.find(path);
    if (!node) throw new VfsError('ENOENT', path);
    return node;
  }

  /**
   * The node a symlink points at, with `count` links followed. `find` and
   * friends are for real paths; this is for reading through a link the way the
   * packer's scripts do (they hand `install -m` a target and a destination).
   */
  resolve(path, count = 0) {
    const node = this.require(path);
    if (node.kind !== 'symlink') return node;
    if (count > 32) throw new VfsError('ELOOP', path);
    const target = node.target.startsWith('/') ? node.target : `${node.parent.path}/${node.target}`;
    return this.resolve(normalise(target), count + 1);
  }

  readdir(path) {
    const node = this.require(path);
    if (!node.children) throw new VfsError('ENOTDIR', path);
    return [...node.children.keys()];
  }

  mkdir(path, mode = 0o755) {
    const { parent, name } = this.split(path);
    if (parent.children.has(name)) return parent.children.get(name);
    const node = new Node({ name, mode: S_IFDIR | (mode & 0o7777), mtime: parent.mtime });
    node.parent = parent;
    parent.children.set(name, node);
    return node;
  }

  /** mkdir -p */
  mkdirTree(path, mode = 0o755) {
    const parts = path.split('/').filter(Boolean);
    let node = this.root;
    let sofar = '';
    for (const part of parts) {
      sofar += `/${part}`;
      node = node.children?.get(part) ?? this.mkdir(sofar, mode);
      if (!node.children) throw new VfsError('ENOTDIR', sofar);
    }
    return node;
  }

  /** Writes a file, creating it and its parents when needed. */
  writeFile(path, bytes, mode = 0o644) {
    const { parent, name } = this.split(path);
    let node = parent.children.get(name);
    if (!node) {
      node = new Node({ name, mode: S_IFREG | (mode & 0o7777), mtime: parent.mtime });
      node.parent = parent;
      parent.children.set(name, node);
    }
    node.mode = S_IFREG | (mode & 0o7777);
    node.children = null;
    node.target = null;
    node.setContent(bytes);
    return node;
  }

  symlink(target, path) {
    const { parent, name } = this.split(path);
    const node = new Node({ name, mode: S_IFLNK | 0o777, mtime: parent.mtime });
    node.parent = parent;
    node.target = target;
    node.size = new TextEncoder().encode(target).length;
    parent.children.set(name, node);
    return node;
  }

  /** chmod, on a real path; a symlink's own mode is never changed. */
  chmod(path, mode) {
    const node = this.find(path);
    if (!node) throw new VfsError('ENOENT', path);
    node.mode = (node.mode & S_IFMT) | (mode & 0o7777);
    return node;
  }

  unlink(path) {
    const { parent, name } = this.split(path);
    const node = parent.children.get(name);
    if (!node) throw new VfsError('ENOENT', path);
    if (node.children) throw new VfsError('EISDIR', path);
    parent.children.delete(name);
    return node;
  }

  /** rm -rf */
  remove(path) {
    const node = this.find(path);
    if (!node) return false;
    node.parent.children.delete(node.name);
    return true;
  }

  split(path) {
    const parts = path.split('/').filter(Boolean);
    const name = parts.pop();
    if (!name) throw new VfsError('EINVAL', path);
    const parent = this.find(`/${parts.join('/')}`);
    if (!parent) throw new VfsError('ENOENT', path);
    if (!parent.children) throw new VfsError('ENOTDIR', path);
    return { parent, name };
  }

  /** Every node, depth first, root first; `path` is '' for the root. */
  *walk(start = this.root) {
    const stack = [start];
    while (stack.length > 0) {
      const node = stack.pop();
      yield node;
      if (node.children) {
        for (const child of [...node.children.values()].reverse()) stack.push(child);
      }
    }
  }

  /** Totals for the page's summary line. */
  summary() {
    const kinds = { file: 0, dir: 0, symlink: 0, other: 0 };
    let bytes = 0;
    for (const node of this.walk()) {
      kinds[node.kind] = (kinds[node.kind] ?? 0) + 1;
      if (node.kind === 'file') bytes += node.size;
    }
    return { ...kinds, bytes };
  }
}

/** Collapses `.` and `..` in an absolute path. */
export function normalise(path) {
  const parts = [];
  for (const part of path.split('/')) {
    if (part === '' || part === '.') continue;
    if (part === '..') parts.pop();
    else parts.push(part);
  }
  return `/${parts.join('/')}`;
}
