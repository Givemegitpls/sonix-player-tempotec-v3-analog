/* `mkfs.ubifs` wants <uuid.h> from util-linux's libuuid, which has no wasm build.
 *
 * It uses exactly two functions: a random v4 UUID for the filesystem's
 * superblock, and its upper-case text form for `--help` and the log line. Both
 * are a dozen lines (src/uuid.c), so libuuid is not worth carrying. The UUID is
 * random in the packer's build too, which is why two builds of one tree differ
 * byte for byte and a browser build is compared by its contents, not its hash.
 */
#ifndef WASM_UUID_H
#define WASM_UUID_H

void uuid_generate_random(unsigned char out[16]);
void uuid_unparse_upper(const unsigned char in[16], char *out);

#endif
