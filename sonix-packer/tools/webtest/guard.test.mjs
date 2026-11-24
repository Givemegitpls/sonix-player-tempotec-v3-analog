// The page's answer to "is this the right image?", as a test.
//
// Runs on plain node -- no wasm, no browser, no firmware fixture: the guard takes
// its expectations as parameters, so a test can compare real bytes against a
// digest it computed itself and, separately, against a digest that cannot match.
// That is the whole point of the parameter: the stock MD5s cannot be forged here,
// but the comparison, the order of the checks and every message can be.
//
//   node sonix-packer/tools/webtest/guard.test.mjs
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const guard = await import(new URL('../../../web/lib/guard.js', import.meta.url).href);
const { md5 } = await import(new URL('../../../web/lib/md5.js', import.meta.url).href);

let failures = 0;
const test = (name, fn) => {
  try {
    fn();
    console.log(`  ok   ${name}`);
  } catch (error) {
    failures += 1;
    console.log(`  FAIL ${name}: ${error.message}`);
  }
};

const encoder = new TextEncoder();
const bytes = (text) => encoder.encode(text);

// An image made of the six files, with a bootloader and a kernel whose digests the
// test knows, and a VERSION.TXT that names the board.
const image = (overrides = {}) => {
  const files = new Map([
    ['SYSTEM.UBI', { data: bytes('rootfs') }],
    ['UBOOT.BIN', { data: bytes('bootloader') }],
    ['UIMAGE.BIN', { data: bytes('kernel') }],
    ['UPDATE.TXT', { data: bytes('kernel={md5=...}') }],
    ['VERSION.TXT', { data: bytes('version={\n name=v3_analog\n}') }],
    ['_GITIGNO', { data: bytes('*.*') }],
  ]);
  for (const [name, data] of Object.entries(overrides)) files.set(name, { data });
  return files;
};

const expect = () => ({ uboot: md5(bytes('bootloader')), kernel: md5(bytes('kernel')), versionName: 'v3_analog' });

console.log('image files');

test('the right image passes and reports both digests', () => {
  const result = guard.checkImageFiles(image(), md5, expect());
  assert.equal(result.uboot, md5(bytes('bootloader')));
  assert.equal(result.kernel, md5(bytes('kernel')));
});

test('a missing file is refused by name', () => {
  const files = image();
  files.delete('SYSTEM.UBI');
  assert.throws(() => guard.checkImageFiles(files, md5, expect()), /no SYSTEM\.UBI/);
});

test("another model's bootloader is refused before anything else", () => {
  const files = image({ 'UBOOT.BIN': bytes('someone else’s bootloader') });
  assert.throws(() => guard.checkImageFiles(files, md5, expect()), /stock bootloader/);
});

test('a matching bootloader with a foreign kernel is refused too', () => {
  const files = image({ 'UIMAGE.BIN': bytes('another kernel') });
  assert.throws(() => guard.checkImageFiles(files, md5, expect()), /stock kernel/);
});

test('a VERSION.TXT that names another board is refused', () => {
  const files = image({ 'VERSION.TXT': bytes('version={\n name=r3proii\n}') });
  assert.throws(() => guard.checkImageFiles(files, md5, expect()), /does not name v3_analog/);
});

console.log('rootfs identity');

const info = (version) => `{
    "device-name": "TempoTec V3 Analog",
    "dac-info": "AKM AK4493SEQ",
    "OS_version": "${version}",
    "build_version": "1234"
}`;

test('the vendor 1.3 for this device passes', () => {
  const result = guard.checkRootfsIdentity(info('1.3.0'));
  assert.equal(result.base, '1.3');
});

test('a ported image ("1.3.0/0.5.0") still answers 1.3', () => {
  assert.equal(guard.checkRootfsIdentity(info('1.3.0/0.5.0')).base, '1.3');
});

test('a newer vendor base is refused with both numbers', () => {
  assert.throws(() => guard.checkRootfsIdentity(info('1.4.0')), /1\.4.*1\.3/);
});

test('an image that names another device is refused', () => {
  const text = info('1.3.0').replace('TempoTec V3 Analog', 'Some Other Player');
  assert.throws(() => guard.checkRootfsIdentity(text), /does not name the TempoTec V3 Analog/);
});

test('an image with no OS_version is refused', () => {
  const text = info('1.3.0').replace(/"OS_version"[^,]*,?/, '');
  assert.throws(() => guard.checkRootfsIdentity(text), /no OS_version/);
});

console.log(failures === 0 ? '\nok' : `\n${failures} failure(s)`);
process.exit(failures === 0 ? 0 : 1);
