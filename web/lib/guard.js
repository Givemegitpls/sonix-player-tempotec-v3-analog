// Is this the vendor's image this port is built on?
//
// One question, two halves, and both have to be answered by the file the user
// dropped: the *model* (TempoTec V3 Analog, not a sibling from the same family)
// and the *vendor version* (1.3, the one this port is built on). A user is never
// asked to choose a model -- the page reads it out of the image, and refuses with
// a sentence that says what it found.
//
// The evidence, cheapest first:
//
//   UBOOT.BIN   the stock bootloader. A wrong one is the only mistake this device
//               cannot undo, so it is checked before anything is parsed further.
//   UIMAGE.BIN  the stock kernel: a second, independent identifier. One digest can
//               in principle belong to more than one model; two that both have to
//               match cannot.
//   VERSION.TXT carries `name=v3_analog`, the vendor's own name for the board.
//   config.json inside the rootfs is the vendor's own product description:
//               `"firmware_name":"v3_analog"`, `"ota_name":"TempoTec V3_ANALOG"` and
//               `"version":"1.3"`. It is the only place the vendor version is
//               stated -- the .upt itself does not carry it -- and the port never
//               overwrites the file, so it answers the same question in a stock
//               image and in one of ours.
//
// Everything here takes its expectations as a parameter with the real values as
// defaults, so the tests can feed their own: an MD5 that has to be *the* stock
// one cannot be forged in a test, but the comparison logic can.
export const STOCK_UBOOT_MD5 = '1f54858d6b4e6f482a81a3378c0e5bf8';
export const STOCK_KERNEL_MD5 = '35889275f8bed2f967a18f6a8b66495c';
export const VENDOR_BASE = '1.3';
export const BOARD_NAME = 'TempoTec V3 Analog';
export const BOARD_ID = 'v3_analog';

/** The six files every .upt of this device has, and nothing else is read. */
export const REQUIRED_FILES = ['SYSTEM.UBI', 'UBOOT.BIN', 'UIMAGE.BIN', 'UPDATE.TXT', 'VERSION.TXT', '_GITIGNO'];

const VENDOR_CONFIG = 'usr/resource/config.json';

export class GuardError extends Error {}

const decoder = new TextDecoder();

/**
 * The model, from the image's own files.
 *
 * @param {Map<string, {data: Uint8Array}>} files the ISO's files
 * @param {(bytes: Uint8Array) => string} md5
 * @param {{uboot?: string, kernel?: string, versionName?: string}} [expect]
 */
export function checkImageFiles(files, md5, expect = {}) {
  const ubootMd5 = expect.uboot ?? STOCK_UBOOT_MD5;
  const kernelMd5 = expect.kernel ?? STOCK_KERNEL_MD5;
  const versionName = expect.versionName ?? BOARD_ID;

  for (const name of REQUIRED_FILES) {
    if (!files.has(name)) {
      throw new GuardError(
        `the image has no ${name}: it is not a ${BOARD_NAME} firmware image (a .upt of another model, or a file that is not one)`,
      );
    }
  }

  const uboot = md5(files.get('UBOOT.BIN').data);
  if (uboot !== ubootMd5) {
    throw new GuardError(
      `this image's UBOOT.BIN is ${uboot}, not the ${BOARD_NAME}'s stock bootloader. ` +
        'It is another model’s firmware, or a vendor version newer than the one this port is built on.',
    );
  }

  const kernel = md5(files.get('UIMAGE.BIN').data);
  if (kernel !== kernelMd5) {
    throw new GuardError(
      `this image's UIMAGE.BIN is ${kernel}, not the ${BOARD_NAME}'s stock kernel. ` +
        'The bootloader matches but the kernel does not: this is not the image this port was built on.',
    );
  }

  const versionTxt = decoder.decode(files.get('VERSION.TXT').data);
  if (!versionTxt.includes(`name=${versionName}`)) {
    throw new GuardError(
      `this image's VERSION.TXT does not name ${versionName}: it is another model’s firmware.`,
    );
  }
  return { uboot, kernel, versionTxt };
}

/**
 * The vendor version, from the product description the rootfs carries.
 *
 * `usr/resource/config.json` is the vendor's own file: an array of records, one of
 * them `{"type":"product","company":"TempoTec","device":"V3","version":"1.3",...}`.
 * The port does not replace it, so a stock image and one of ours answer the same
 * question -- which vendor firmware is this built on -- and this is the only place
 * the answer exists: neither the `.upt` nor `VERSION.TXT` states "1.3".
 *
 * `version` is "1.3" on the stock file and stays so in images this port packs; a
 * "1.4" would be a base nobody has ported.
 *
 * @param {string} text the contents of `usr/resource/config.json`
 * @param {{firmwareName?: string, otaName?: string, base?: string}} [expect]
 */
export function checkVendorConfig(text, expect = {}) {
  const firmwareName = expect.firmwareName ?? BOARD_ID;
  const otaName = expect.otaName ?? 'TempoTec V3_ANALOG';
  const base = expect.base ?? VENDOR_BASE;

  let records;
  try {
    records = JSON.parse(text);
  } catch {
    throw new GuardError('this rootfs has no readable usr/resource/config.json: it is not a vendor image this port can build on.');
  }
  if (!Array.isArray(records)) {
    throw new GuardError('this rootfs\'s usr/resource/config.json is not the vendor\'s product list.');
  }
  const product = records.find((record) => record?.type === 'product');
  if (!product) {
    throw new GuardError('this rootfs\'s config.json has no product record: it is not a vendor image this port can build on.');
  }
  if (product.firmware_name !== firmwareName || product.ota_name !== otaName) {
    throw new GuardError(
      `this image is the vendor firmware "${product.firmware_name ?? '?'}" (${product.ota_name ?? '?'}), ` +
        `not ${firmwareName} (${otaName}): it is another model's firmware.`,
    );
  }

  const found = String(product.version ?? '');
  const foundBase = found.split('/')[0].split('.').slice(0, 2).join('.');
  if (foundBase !== base) {
    throw new GuardError(
      `this image is the vendor's ${foundBase || '?'} firmware; this port is built on ${base}. ` +
        'Take the 1.3 image for this device: a newer vendor base has not been ported.',
    );
  }
  return { firmwareName, otaName, base: foundBase, version: found };
}

/** Where the guard reads the vendor's version from, for the caller's own checks. */
export const VENDOR_CONFIG_PATH = VENDOR_CONFIG;
