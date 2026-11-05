#ifndef NETDNS_H
#define NETDNS_H

#include <stdbool.h>
#include <stddef.h>

// The addresses the resolver uses, and the two the user may put in their place.
//
// The DHCP client writes /etc/resolv.conf when Wi-Fi associates, and that file
// is the whole of the resolver's configuration on this device: glibc reads it
// on the first lookup and (2.22, which predates the version that follows the
// file's timestamp) keeps what it found. http.c re-reads it when it moves --
// see dns_follow_resolv_conf() -- so anything written here is picked up by the
// next request without a restart.
//
// The override is kept in device_config.ini under [net] dns1/dns2 and is put
// back at the top of the file after every lease. The DHCP client's own lines
// (`nameserver ... # wlan0`) are never touched: udhcpc's script strips and
// re-appends only lines carrying that suffix, so the two kinds sit in one file
// without either erasing the other, and ours -- the ones written first -- win.

#define NETDNS_ADDRESS_MAX 16

// The addresses in use: the override when one is set for that slot, otherwise
// the DHCP-provided one. Empty strings when there is neither.
void netdns_get(char primary[NETDNS_ADDRESS_MAX], char secondary[NETDNS_ADDRESS_MAX]);

// A dotted-quad IPv4 address, which is all the user is ever asked to type.
bool netdns_is_address(const char *text);

// Remembers the override (an empty string clears that slot) and rewrites
// /etc/resolv.conf. False when the file could not be written.
bool netdns_set(const char *primary, const char *secondary);

// Puts the stored override back at the top of /etc/resolv.conf, dropping a
// previous copy of it. Called after every DHCP lease and once at start-up; a
// no-op beyond one small file write when nothing is set.
void netdns_apply(void);

#endif /* NETDNS_H */
