// hciwatch -- read the controller's HCI traffic and decode the L2CAP channels.
//
// Why this exists: when the A2DP receiver goes silent the player's own log and
// bluetoothd's debug log both stop at the AVDTP layer -- they say the stream
// reached STREAMING and that a transport fd was handed out, and nothing about
// what the phone then does on the air. The answer is one level below: the
// L2CAP media channel's MTU, mode and flush timeout, and whether any ACL data
// ever arrives on it. On a 3.10 kernel there is no btmon, but there *is* the
// HCI monitor channel (HCI_CHANNEL_MONITOR, the same one btmon uses on newer
// kernels), which hands out every command, event and ACL packet without taking
// the controller away from bluetoothd.
//
//   hciwatch [seconds]            # default: run until killed
//
// What it prints, per line, one packet:
//   MON NEW_INDEX ...            adapter registration
//   CMD 0xXXXX ...               HCI command
//   EVT 0xXX ...                 HCI event
//   RX/TX acl h=0xXXX len=N ...  ACL packet, with the L2CAP header decoded
// and, for L2CAP signalling (CID 0x0001), the CONN/CONFIG request and response
// fields -- PSM, SCID/DCID, result, and the CONFIG options (MTU, flush timeout,
// retransmission/ERTM mode). Data on a dynamic channel (the media channel) is
// only counted, with a running total printed every 5 s, because that is what
// answers "does the phone transmit at all".
//
// Build (against the device's libbluetooth is not needed -- this is plain
// sockets, so only libc):
//   rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-gcc -O2 -o hciwatch hciwatch.c
//
// The monitor channel needs CAP_NET_RAW, i.e. root; on the device everything
// runs as root. If the vendor kernel was built without it (bind fails with
// EINVAL/EPERM), -r switches to a raw HCI socket, which the 3.10 kernel also
// delivers ACL to -- but a raw socket is per-adapter and its default filter
// hides everything but events, so the filter is opened up here.

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/l2cap.h>

// The monitor channel's framing is stable since it was introduced, but the
// headers in the cross sysroot predate it, so define it here.
#define HCI_MON_HDR_SIZE	6
struct hci_mon_hdr {
	uint16_t opcode;
	uint16_t index;
	uint16_t len;
} __attribute__((packed));

#define HCI_MON_NEW_INDEX	0
#define HCI_MON_DEL_INDEX	1
#define HCI_MON_COMMAND_PKT	2
#define HCI_MON_EVENT_PKT	3
#define HCI_MON_ACL_TX_PKT	4
#define HCI_MON_ACL_RX_PKT	5
#define HCI_MON_SCO_TX_PKT	6
#define HCI_MON_SCO_RX_PKT	7

// L2CAP signalling (CID 0x0001) command codes.
#define L2CAP_CID_SIG		0x0001
#define L2CAP_CONN_REQ		0x02
#define L2CAP_CONN_RSP		0x03
#define L2CAP_CONFIG_REQ	0x04
#define L2CAP_CONFIG_RSP	0x05
#define L2CAP_DISCONN_REQ	0x06
#define L2CAP_DISCONN_RSP	0x07
#define L2CAP_INFO_REQ		0x0a
#define L2CAP_INFO_RSP		0x0b

// A dynamic channel's traffic is counted per CID, so the summary can say which
// channel carried bytes: the AVDTP signalling channel (0x0040-ish), the media
// channel, or an AVRCP channel.
#define MAX_CHANNELS		16
struct channel_stat {
	uint16_t cid;
	unsigned long rx_bytes;
	unsigned long tx_bytes;
	unsigned long rx_pkts;
	unsigned long tx_pkts;
};
static struct channel_stat channels[MAX_CHANNELS];

static int monitor_sock = -1;
static volatile int running = 1;

// gettimeofday() rather than clock_gettime(): on this toolchain clock_gettime
// is versioned GLIBC_2.17, which the device's glibc 2.16 does not export, and
// the sniffer has no need for a monotonic clock.
static double now_seconds(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static struct channel_stat *channel_for(uint16_t cid) {
	struct channel_stat *free_slot = NULL;
	for (size_t i = 0; i < MAX_CHANNELS; i++) {
		if (channels[i].cid == cid) {
			return &channels[i];
		}
		if (channels[i].cid == 0 && free_slot == NULL) {
			free_slot = &channels[i];
		}
	}
	if (free_slot != NULL) {
		free_slot->cid = cid;
	}
	return free_slot;
}

// Print the L2CAP CONFIG options: these are the bytes that decide the media
// channel's MTU, flush timeout and L2CAP mode, i.e. the exact things a wrong
// version of either stack can get wrong while still reaching STREAMING.
static void print_config_options(const uint8_t *p, int len) {
	while (len >= 2) {
		uint8_t type = p[0];
		uint8_t olen = p[1];
		if (olen + 2 > len) {
			printf(" opt(truncated type=0x%02x len=%u)", type, olen);
			return;
		}
		switch (type) {
		case 0x01:
			if (olen == 2) {
				uint16_t mtu;
				memcpy(&mtu, p + 2, 2);
				printf(" mtu=%u", mtu);
			}
			break;
		case 0x02:
			if (olen == 2) {
				uint16_t flush;
				memcpy(&flush, p + 2, 2);
				printf(" flush_timeout=0x%04x", flush);
			}
			break;
		case 0x04:
			if (olen >= 1) {
				printf(" mode=%u", p[2]);
			}
			break;
		case 0x05:
			if (olen == 1) {
				printf(" fcs=%u", p[2]);
			}
			break;
		default:
			printf(" opt(type=0x%02x len=%u)", type, olen);
			break;
		}
		p += olen + 2;
		len -= olen + 2;
	}
}

static void decode_signalling(const uint8_t *p, int len, const char *dir) {
	while (len >= 4) {
		uint8_t code = p[0];
		uint8_t ident = p[1];
		uint16_t clen;
		memcpy(&clen, p + 2, 2);
		if (clen + 4 > len) {
			printf(" %s sig(truncated code=0x%02x)", dir, code);
			return;
		}
		const uint8_t *v = p + 4;

		switch (code) {
		case L2CAP_CONN_REQ: {
			uint16_t psm, scid;
			memcpy(&psm, v, 2);
			memcpy(&scid, v + 2, 2);
			printf(" %s CONN_REQ ident=%u psm=0x%04x scid=0x%04x", dir, ident, psm, scid);
			break;
		}
		case L2CAP_CONN_RSP: {
			uint16_t dcid, scid, result;
			memcpy(&dcid, v, 2);
			memcpy(&scid, v + 2, 2);
			memcpy(&result, v + 4, 2);
			printf(" %s CONN_RSP ident=%u dcid=0x%04x scid=0x%04x result=0x%04x", dir, ident, dcid, scid, result);
			break;
		}
		case L2CAP_CONFIG_REQ: {
			uint16_t dcid, flags;
			memcpy(&dcid, v, 2);
			memcpy(&flags, v + 2, 2);
			printf(" %s CONFIG_REQ ident=%u dcid=0x%04x flags=0x%04x", dir, ident, dcid, flags);
			print_config_options(v + 4, clen - 4);
			break;
		}
		case L2CAP_CONFIG_RSP: {
			uint16_t scid, flags, result;
			memcpy(&scid, v, 2);
			memcpy(&flags, v + 2, 2);
			memcpy(&result, v + 4, 2);
			printf(" %s CONFIG_RSP ident=%u scid=0x%04x flags=0x%04x result=0x%04x", dir, ident, scid, flags, result);
			if (clen > 6) {
				print_config_options(v + 6, clen - 6);
			}
			break;
		}
		case L2CAP_DISCONN_REQ:
		case L2CAP_DISCONN_RSP: {
			uint16_t dcid, scid;
			memcpy(&dcid, v, 2);
			memcpy(&scid, v + 2, 2);
			printf(" %s %s ident=%u dcid=0x%04x scid=0x%04x", dir,
					code == L2CAP_DISCONN_REQ ? "DISCONN_REQ" : "DISCONN_RSP", ident, dcid, scid);
			break;
		}
		default:
			printf(" %s SIG code=0x%02x ident=%u len=%u", dir, code, ident, clen);
			break;
		}

		p += clen + 4;
		len -= clen + 4;
	}
}

static void decode_acl(const uint8_t *p, int len, const char *dir) {
	if (len < 4) {
		printf("%s acl (short)", dir);
		return;
	}
	uint16_t handle_pb;
	uint16_t dlen;
	memcpy(&handle_pb, p, 2);
	memcpy(&dlen, p + 2, 2);
	uint16_t handle = handle_pb & 0x0fff;

	printf("%s acl h=0x%03x len=%u", dir, handle, dlen);

	if (dlen + 4 > len || dlen < 4) {
		return;
	}

	const uint8_t *l = p + 4;
	uint16_t llen, cid;
	memcpy(&llen, l, 2);
	memcpy(&cid, l + 2, 2);

	struct channel_stat *st = channel_for(cid);
	if (st != NULL) {
		if (dir[0] == 'R') {
			st->rx_bytes += dlen;
			st->rx_pkts++;
		} else {
			st->tx_bytes += dlen;
			st->tx_pkts++;
		}
	}

	printf(" cid=0x%04x l2len=%u", cid, llen);

	if (cid == L2CAP_CID_SIG) {
		decode_signalling(l + 4, llen < dlen - 4 ? llen : dlen - 4, dir);
	}
}

static void decode_monitor(const uint8_t *buf, int n) {
	if (n < HCI_MON_HDR_SIZE) {
		return;
	}
	const struct hci_mon_hdr *h = (const struct hci_mon_hdr *)buf;
	uint16_t opcode = h->opcode;
	uint16_t index = h->index;
	uint16_t len = h->len;
	if (HCI_MON_HDR_SIZE + len > n) {
		len = n - HCI_MON_HDR_SIZE;
	}
	const uint8_t *p = buf + HCI_MON_HDR_SIZE;

	printf("%7.3f ", now_seconds());

	switch (opcode) {
	case HCI_MON_NEW_INDEX: {
		char addr[18] = "";
		if (len >= 7) {
			ba2str((const bdaddr_t *)(p + 2), addr);
		}
		printf("MON NEW_INDEX index=%u addr=%s", index, addr);
		break;
	}
	case HCI_MON_DEL_INDEX:
		printf("MON DEL_INDEX index=%u", index);
		break;
	case HCI_MON_COMMAND_PKT: {
		uint16_t oc = 0;
		if (len >= 2) {
			memcpy(&oc, p, 2);
		}
		printf("CMD index=%u opcode=0x%04x len=%u", index, oc, len);
		break;
	}
	case HCI_MON_EVENT_PKT: {
		unsigned ev = len > 0 ? p[0] : 0;
		printf("EVT index=%u event=0x%02x len=%u", index, ev, len);
		break;
	}
	case HCI_MON_ACL_RX_PKT:
		decode_acl(p, len, "RX");
		break;
	case HCI_MON_ACL_TX_PKT:
		decode_acl(p, len, "TX");
		break;
	case HCI_MON_SCO_RX_PKT:
		printf("RX sco index=%u len=%u", index, len);
		break;
	case HCI_MON_SCO_TX_PKT:
		printf("TX sco index=%u len=%u", index, len);
		break;
	default:
		printf("MON opcode=%u index=%u len=%u", opcode, index, len);
		break;
	}
	putchar('\n');
	fflush(stdout);
}

static void on_signal(int sig) {
	(void)sig;
	running = 0;
}

// A raw HCI socket: per-adapter, and its default filter drops everything but
// events, so the filter is widened to all packet types before reading.
static int open_raw(const char *dev) {
	int id = hci_get_route(NULL);
	if (id < 0) {
		return -1;
	}
	int s = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
	if (s < 0) {
		return -1;
	}
	// hci_filter_all_ptypes()/hci_filter_all_events() are not exported by the
	// device's libbluetooth (it was built with the tools disabled), so the
	// filter is filled in by hand: every packet type, every event.
	struct hci_filter flt;
	memset(&flt, 0, sizeof(flt));
	flt.type_mask = 0xffffffff;
	flt.event_mask[0] = 0xffffffff;
	flt.event_mask[1] = 0xffffffff;
	if (setsockopt(s, SOL_HCI, HCI_FILTER, &flt, sizeof(flt)) < 0) {
		close(s);
		return -1;
	}
	struct sockaddr_hci addr;
	memset(&addr, 0, sizeof(addr));
	addr.hci_family = AF_BLUETOOTH;
	addr.hci_dev = id;
	addr.hci_channel = HCI_CHANNEL_RAW;
	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(s);
		return -1;
	}
	(void)dev;
	return s;
}

static int open_monitor(void) {
	int s = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
	if (s < 0) {
		return -1;
	}
	// A2DP media is high-rate; the monitor socket drops what it cannot queue,
	// so give it room before binding.
	int rcvbuf = 4 * 1024 * 1024;
	setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
	struct sockaddr_hci addr;
	memset(&addr, 0, sizeof(addr));
	addr.hci_family = AF_BLUETOOTH;
	addr.hci_dev = HCI_DEV_NONE;
	addr.hci_channel = HCI_CHANNEL_MONITOR;
	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		int err = errno;
		close(s);
		errno = err;
		return -1;
	}
	return s;
}

static void print_summary(void) {
	printf("# channel summary: cid rx_pkts rx_bytes tx_pkts tx_bytes\n");
	for (size_t i = 0; i < MAX_CHANNELS; i++) {
		if (channels[i].cid == 0) {
			continue;
		}
		printf("#  0x%04x %lu %lu %lu %lu\n", channels[i].cid,
				channels[i].rx_pkts, channels[i].rx_bytes,
				channels[i].tx_pkts, channels[i].tx_bytes);
	}
	fflush(stdout);
}

int main(int argc, char **argv) {
	int seconds = 0;
	int raw = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-r") == 0) {
			raw = 1;
		} else {
			seconds = atoi(argv[i]);
		}
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	monitor_sock = raw ? open_raw(NULL) : open_monitor();
	if (monitor_sock < 0) {
		if (!raw) {
			fprintf(stderr, "hciwatch: monitor channel unavailable (%s); retrying raw\n", strerror(errno));
			monitor_sock = open_raw(NULL);
			if (monitor_sock >= 0) {
				raw = 1;
			}
		}
	}
	if (monitor_sock < 0) {
		fprintf(stderr, "hciwatch: cannot watch the controller: %s\n", strerror(errno));
		return 1;
	}

	printf("# hciwatch: %s, %s\n", raw ? "raw HCI" : "HCI monitor channel",
			seconds > 0 ? "bounded run" : "running until killed");
	fflush(stdout);

	double started = now_seconds();
	double last_summary = started;

	while (running) {
		if (seconds > 0 && now_seconds() - started >= seconds) {
			break;
		}

		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(monitor_sock, &fds);
		struct timeval tv = { 1, 0 };
		int r = select(monitor_sock + 1, &fds, NULL, NULL, &tv);
		if (r < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}

		double now = now_seconds();
		if (now - last_summary >= 5.0) {
			last_summary = now;
			print_summary();
		}

		if (r == 0) {
			continue;
		}

		uint8_t buf[4096];
		int n = (int)read(monitor_sock, buf, sizeof(buf));
		if (n <= 0) {
			continue;
		}
		if (raw) {
			// A raw socket delivers the packet with its type byte first, but
			// no monitor header, so wrap it into the same shape.
			struct hci_mon_hdr h;
			h.opcode = 0xffff;	// decoded below from the type byte
			h.index = 0;
			h.len = (uint16_t)(n - 1);
			uint8_t wrapped[4200];
			memcpy(wrapped, &h, HCI_MON_HDR_SIZE);
			memcpy(wrapped + HCI_MON_HDR_SIZE, buf + 1, n - 1);
			switch (buf[0]) {
			case HCI_COMMAND_PKT:
				((struct hci_mon_hdr *)wrapped)->opcode = HCI_MON_COMMAND_PKT;
				break;
			case HCI_EVENT_PKT:
				((struct hci_mon_hdr *)wrapped)->opcode = HCI_MON_EVENT_PKT;
				break;
			case HCI_ACLDATA_PKT:
				((struct hci_mon_hdr *)wrapped)->opcode = HCI_MON_ACL_RX_PKT;
				break;
			case HCI_SCODATA_PKT:
				((struct hci_mon_hdr *)wrapped)->opcode = HCI_MON_SCO_RX_PKT;
				break;
			default:
				continue;
			}
			decode_monitor(wrapped, HCI_MON_HDR_SIZE + n - 1);
		} else {
			decode_monitor(buf, n);
		}
	}

	print_summary();
	close(monitor_sock);
	return 0;
}
