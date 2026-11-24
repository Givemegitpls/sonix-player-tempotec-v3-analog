/* The two libuuid calls mkfs.ubifs makes, in a dozen lines.
 *
 * A v4 UUID out of `rand()`: the browser has no /dev/urandom to hand to a wasm
 * build, and the value is used the way the packer's mkfs.ubifs uses it -- as an
 * identifier no two filesystems share, not as a secret. The text form is the
 * upper-case 8-4-4-4-12 that uuid_unparse_upper produces.
 */
#include <stdio.h>
#include <stdlib.h>

#include "uuid.h"

void uuid_generate_random(unsigned char out[16])
{
	int i;

	for (i = 0; i < 16; i++)
		out[i] = (unsigned char)(rand() & 0xff);
	out[6] = (unsigned char)((out[6] & 0x0f) | 0x40); /* version 4 */
	out[8] = (unsigned char)((out[8] & 0x3f) | 0x80); /* RFC 4122 variant */
}

void uuid_unparse_upper(const unsigned char in[16], char *out)
{
	sprintf(out, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
		in[0], in[1], in[2], in[3], in[4], in[5], in[6], in[7],
		in[8], in[9], in[10], in[11], in[12], in[13], in[14], in[15]);
}
