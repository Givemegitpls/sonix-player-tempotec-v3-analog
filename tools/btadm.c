// btadm -- drive BlueZ 5 over D-Bus: what bluetoothctl would do, without readline.
//
// Why this exists: on this device bluetoothctl is not built (it needs readline)
// and the firmware carries BlueZ 4.101, so nothing on the device can pair,
// connect or make the adapter discoverable by hand. Everything the receiver
// tests need -- the adapter's Discoverable/Pairable flags, discovery, the
// pairing agent that answers the phone, the connect that follows -- is on
// org.bluez and is one small D-Bus call each.
//
//   btadm adapters                       the adapters and their flags
//   btadm devices                        every device the adapter knows
//   btadm set <prop> <value>             powered/discoverable/pairable/alias/...
//   btadm discover <seconds>             start discovery, name what turns up
//   btadm pair <MAC>                     Device1.Pair
//   btadm connect <MAC>                  Device1.Connect
//   btadm disconnect <MAC>               Device1.Disconnect
//   btadm trust <MAC> <yes|no>           Device1.Trusted
//   btadm remove <MAC>                   Adapter1.RemoveDevice
//   btadm wait <MAC> <seconds>           watch one device's state, printing changes
//   btadm agent [capability]             answer pairing requests in the foreground
//
// The agent is the piece the headless receiver tests need: the player registers
// one of its own, so with the player stopped nothing would answer the phone.
// It is registered as NoInputNoOutput -- just works, no passkey, no numeric
// comparison -- which is what makes pairing a phone to this device need no tap
// on the device at all.
//
// Build (against the device's own libdbus, from this directory):
//   rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-gcc -O2 -o btadm btadm.c
//       -I<sysroot>/usr/include/dbus-1.0 -I<sysroot>/usr/lib/dbus-1.0/include
//       -L<sysroot>/usr/lib -ldbus-1

#include <dbus/dbus.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static DBusConnection *conn;
static DBusError err;

// ---------------------------------------------------------------- properties

// The value of one string property, or NULL. The returned pointer belongs to
// the iterator and stays valid until it is consumed. Four rotating buffers,
// because a printf that asks for several properties at once would otherwise
// print the last one four times.
static const char *prop_string(DBusMessageIter *props, const char *name) {
	DBusMessageIter dict = *props;
	static char bufs[4][256];
	static unsigned next_buf;
	char *buf = bufs[next_buf++ % 4];

	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, value;
		const char *key = NULL;

		dbus_message_iter_recurse(&dict, &entry);
		dbus_message_iter_get_basic(&entry, &key);
		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &value);

		if (key && strcmp(key, name) == 0) {
			const char *s = NULL;
			if (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_STRING) {
				dbus_message_iter_get_basic(&value, &s);
				snprintf(buf, 256, "%s", s ? s : "");
				return buf;
			}
			return NULL;
		}
		dbus_message_iter_next(&dict);
	}
	return NULL;
}

static bool prop_bool(DBusMessageIter *props, const char *name, bool fallback) {
	DBusMessageIter dict = *props;

	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, value;
		const char *key = NULL;

		dbus_message_iter_recurse(&dict, &entry);
		dbus_message_iter_get_basic(&entry, &key);
		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &value);

		if (key && strcmp(key, name) == 0 && dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_BOOLEAN) {
			dbus_bool_t b = 0;
			dbus_message_iter_get_basic(&value, &b);
			return b != 0;
		}
		dbus_message_iter_next(&dict);
	}
	return fallback;
}

// The properties dictionary of `iface` inside one object of the reply.
static bool iface_props(DBusMessageIter *interfaces, const char *iface, DBusMessageIter *out) {
	DBusMessageIter it = *interfaces;

	while (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, props;
		const char *key = NULL;

		dbus_message_iter_recurse(&it, &entry);
		dbus_message_iter_get_basic(&entry, &key);
		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &props);

		if (key && strcmp(key, iface) == 0) {
			*out = props;
			return true;
		}
		dbus_message_iter_next(&it);
	}
	return false;
}

// --------------------------------------------------------------- bus helpers

static DBusMessage *managed_objects_on(const char *service, unsigned timeout_ms) {
	DBusMessage *msg = dbus_message_new_method_call(service, "/", "org.freedesktop.DBus.ObjectManager",
													"GetManagedObjects");
	if (!msg) {
		return NULL;
	}
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, timeout_ms, &err);
	dbus_message_unref(msg);
	return reply;
}

static DBusMessage *managed_objects(unsigned timeout_ms) {
	return managed_objects_on("org.bluez", timeout_ms);
}

// Walk every object of a GetManagedObjects reply, handing the path and the
// properties of `iface` to the caller.
typedef void (*object_fn)(const char *path, DBusMessageIter *props, void *user);

static int walk_objects(DBusMessage *reply, const char *iface, object_fn fn, void *user) {
	DBusMessageIter root, objects;

	if (!dbus_message_iter_init(reply, &root) || dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_ARRAY) {
		return -1;
	}
	dbus_message_iter_recurse(&root, &objects);

	int found = 0;
	while (dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, interfaces, props;
		const char *path = NULL;

		dbus_message_iter_recurse(&objects, &entry);
		dbus_message_iter_get_basic(&entry, &path);
		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &interfaces);

		if (iface_props(&interfaces, iface, &props)) {
			found++;
			if (fn) {
				fn(path, &props, user);
			}
		}
		dbus_message_iter_next(&objects);
	}
	return found;
}

static char adapter_path_buf[256];

static void take_adapter_path(const char *path, DBusMessageIter *props, void *user) {
	(void)props;
	(void)user;
	snprintf(adapter_path_buf, sizeof(adapter_path_buf), "%s", path);
}

static const char *adapter_path(void) {
	if (adapter_path_buf[0]) {
		return adapter_path_buf;
	}
	DBusMessage *reply = managed_objects(5000);
	if (!reply) {
		fprintf(stderr, "btadm: org.bluez did not answer: %s\n", err.message ? err.message : "?");
		return NULL;
	}
	walk_objects(reply, "org.bluez.Adapter1", take_adapter_path, NULL);
	dbus_message_unref(reply);
	return adapter_path_buf[0] ? adapter_path_buf : NULL;
}

static char device_path_buf[256];

static char *mac_to_path(const char *mac, char *out, size_t outlen) {
	snprintf(out, outlen, "/org/bluez/hci0/dev_%s", mac);
	for (char *p = out; *p; p++) {
		if (*p == ':') {
			*p = '_';
		}
	}
	return out;
}

struct find_ctx {
	const char *mac;
	char *out;
	size_t outlen;
};

static void find_device(const char *path, DBusMessageIter *props, void *user) {
	struct find_ctx *ctx = user;
	const char *addr = prop_string(props, "Address");
	if (addr && ctx->mac && strcasecmp(addr, ctx->mac) == 0) {
		snprintf(ctx->out, ctx->outlen, "%s", path);
	}
}

static const char *device_path(const char *mac) {
	// The path BlueZ would use is also the one it lists; ask the bus so a
	// device BlueZ knows under another index still resolves.
	DBusMessage *reply = managed_objects(5000);
	if (reply) {
		struct find_ctx ctx = {mac, device_path_buf, sizeof(device_path_buf)};
		device_path_buf[0] = 0;
		walk_objects(reply, "org.bluez.Device1", find_device, &ctx);
		dbus_message_unref(reply);
	}
	if (!device_path_buf[0]) {
		mac_to_path(mac, device_path_buf, sizeof(device_path_buf));
	}
	return device_path_buf;
}

static int call_method(const char *path, const char *iface, const char *method, int argc, char **argv, bool print) {
	DBusMessage *msg = dbus_message_new_method_call("org.bluez", path, iface, method);
	if (!msg) {
		fprintf(stderr, "btadm: out of memory\n");
		return 1;
	}
	for (int i = 0; i < argc; i++) {
		const char *s = argv[i];
		if (!dbus_message_append_args(msg, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID)) {
			fprintf(stderr, "btadm: cannot append %s\n", s);
			dbus_message_unref(msg);
			return 1;
		}
	}
	DBusError call_err;
	dbus_error_init(&call_err);
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 30000, &call_err);
	dbus_message_unref(msg);
	if (!reply) {
		fprintf(stderr, "btadm: %s failed: %s\n", method, call_err.message ? call_err.message : "?");
		dbus_error_free(&call_err);
		return 1;
	}
	if (print) {
		printf("%s: ok\n", method);
	}
	dbus_message_unref(reply);
	return 0;
}

static int set_property(const char *path, const char *iface, const char *name, const char *value) {
	DBusMessage *msg = dbus_message_new_method_call("org.bluez", path, "org.freedesktop.DBus.Properties",
													"SetProperty");
	if (!msg) {
		return 1;
	}
	DBusMessageIter it, var;
	dbus_message_iter_init_append(msg, &it);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &name);
	dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "b", &var);
	dbus_bool_t b = strcmp(value, "false") != 0 && strcmp(value, "0") != 0 && strcmp(value, "no") != 0;
	dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &b);
	dbus_message_iter_close_container(&it, &var);
	(void)value;

	DBusError call_err;
	dbus_error_init(&call_err);
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 10000, &call_err);
	dbus_message_unref(msg);
	if (!reply) {
		fprintf(stderr, "btadm: SetProperty %s failed: %s\n", name, call_err.message ? call_err.message : "?");
		dbus_error_free(&call_err);
		return 1;
	}
	dbus_message_unref(reply);
	printf("%s = %s\n", name, b ? "true" : "false");
	return 0;
}

// ------------------------------------------------------------------- listing

static void print_adapter(const char *path, DBusMessageIter *props, void *user) {
	(void)user;
	printf("adapter %s: %s  alias=%s  powered=%s  discoverable=%s  pairable=%s  discovering=%s\n", path,
		   prop_string(props, "Address") ? prop_string(props, "Address") : "?",
		   prop_string(props, "Alias") ? prop_string(props, "Alias") : "?",
		   prop_bool(props, "Powered", false) ? "yes" : "no",
		   prop_bool(props, "Discoverable", false) ? "yes" : "no",
		   prop_bool(props, "Pairable", false) ? "yes" : "no",
		   prop_bool(props, "Discovering", false) ? "yes" : "no");
}

static void print_device(const char *path, DBusMessageIter *props, void *user) {
	(void)user;
	printf("  %s  %s  name=%s  paired=%s  bonded=%s  connected=%s  trusted=%s\n", path,
		   prop_string(props, "Address") ? prop_string(props, "Address") : "?",
		   prop_string(props, "Alias") ? prop_string(props, "Alias") : "?",
		   prop_bool(props, "Paired", false) ? "yes" : "no",
		   prop_bool(props, "Bonded", false) ? "yes" : "no",
		   prop_bool(props, "Connected", false) ? "yes" : "no",
		   prop_bool(props, "Trusted", false) ? "yes" : "no");
}

static int cmd_list(bool devices) {
	DBusMessage *reply = managed_objects(5000);
	if (!reply) {
		fprintf(stderr, "btadm: org.bluez did not answer: %s\n", err.message ? err.message : "?");
		return 1;
	}
	int adapters = walk_objects(reply, "org.bluez.Adapter1", print_adapter, NULL);
	if (devices) {
		walk_objects(reply, "org.bluez.Device1", print_device, NULL);
	}
	dbus_message_unref(reply);
	return adapters > 0 ? 0 : 2;
}

// ----------------------------------------------------------------- discovery

struct seen {
	char paths[64][128];
	int count;
};

static void note_device(const char *path, DBusMessageIter *props, void *user) {
	struct seen *seen = user;
	for (int i = 0; i < seen->count; i++) {
		if (strcmp(seen->paths[i], path) == 0) {
			return;
		}
	}
	if (seen->count >= 64) {
		return;
	}
	snprintf(seen->paths[seen->count], sizeof(seen->paths[0]), "%s", path);
	seen->count++;
	print_device(path, props, NULL);
	fflush(stdout);
}

static int cmd_discover(int seconds) {
	const char *adapter = adapter_path();
	if (!adapter) {
		return 1;
	}
	set_property(adapter, "org.bluez.Adapter1", "Powered", "true");
	if (call_method(adapter, "org.bluez.Adapter1", "StartDiscovery", 0, NULL, false) != 0) {
		return 1;
	}
	printf("discovery on for %d s\n", seconds);

	struct seen seen;
	memset(&seen, 0, sizeof(seen));

	for (int i = 0; i < seconds; i++) {
		sleep(1);
		DBusMessage *reply = managed_objects(3000);
		if (!reply) {
			continue;
		}
		walk_objects(reply, "org.bluez.Device1", note_device, &seen);
		dbus_message_unref(reply);
	}
	call_method(adapter, "org.bluez.Adapter1", "StopDiscovery", 0, NULL, false);
	return 0;
}

// --------------------------------------------------------------------- agent

// The agent interface, answered from one place: every request is accepted and
// every display request is acknowledged with nothing.
static const char *agent_capability = "NoInputNoOutput";

static const char *short_signal(const char *member) {
	if (strcmp(member, "RequestConfirmation") == 0) {
		return "confirmation (passkey shown on the other side is dismissed)";
	}
	if (strcmp(member, "RequestAuthorization") == 0) {
		return "authorization";
	}
	if (strcmp(member, "AuthorizeService") == 0) {
		return "service authorization";
	}
	if (strcmp(member, "RequestPinCode") == 0) {
		return "PIN code (0000)";
	}
	if (strcmp(member, "RequestPasskey") == 0) {
		return "passkey (0)";
	}
	if (strcmp(member, "DisplayPasskey") == 0) {
		return "passkey display";
	}
	if (strcmp(member, "DisplayPinCode") == 0) {
		return "PIN display";
	}
	if (strcmp(member, "Release") == 0) {
		return "release";
	}
	if (strcmp(member, "Cancel") == 0) {
		return "cancel";
	}
	return member;
}

static DBusHandlerResult agent_handler(DBusConnection *c, DBusMessage *msg, void *user) {
	(void)user;
	if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL) {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	const char *iface = dbus_message_get_interface(msg);
	const char *member = dbus_message_get_member(msg);
	if (!iface || strcmp(iface, "org.bluez.Agent1") != 0) {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	DBusMessage *reply = dbus_message_new_method_return(msg);
	if (!reply) {
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	}

	if (strcmp(member, "RequestPinCode") == 0) {
		const char *pin = "0000";
		dbus_message_append_args(reply, DBUS_TYPE_STRING, &pin, DBUS_TYPE_INVALID);
	} else if (strcmp(member, "RequestPasskey") == 0) {
		dbus_uint32_t passkey = 0;
		dbus_message_append_args(reply, DBUS_TYPE_UINT32, &passkey, DBUS_TYPE_INVALID);
	}
	printf("agent: %s -> accepted\n", short_signal(member ? member : "?"));
	fflush(stdout);
	dbus_connection_send(c, reply, NULL);
	dbus_message_unref(reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static int cmd_agent(const char *capability) {
	const char *adapter = adapter_path();
	if (!adapter) {
		return 1;
	}

	const char *path = "/org/sonix/btadm/agent";
	if (!dbus_connection_register_object_path(conn, path, &(DBusObjectPathVTable){.message_function = agent_handler},
											  NULL)) {
		fprintf(stderr, "btadm: cannot take %s on the bus\n", path);
		return 1;
	}

	// RegisterAgent(path, capability): an agent the bus can reach and the
	// daemon can call, then make it the default one.
	DBusMessage *msg = dbus_message_new_method_call("org.bluez", "/org/bluez", "org.bluez.AgentManager1",
													"RegisterAgent");
	dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_STRING, &capability, DBUS_TYPE_INVALID);
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 10000, &err);
	dbus_message_unref(msg);
	if (!reply) {
		fprintf(stderr, "btadm: RegisterAgent failed: %s\n", err.message ? err.message : "?");
		return 1;
	}
	dbus_message_unref(reply);

	msg = dbus_message_new_method_call("org.bluez", "/org/bluez", "org.bluez.AgentManager1", "RequestDefaultAgent");
	dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID);
	reply = dbus_connection_send_with_reply_and_block(conn, msg, 10000, &err);
	dbus_message_unref(msg);
	if (!reply) {
		fprintf(stderr, "btadm: RequestDefaultAgent failed: %s (continuing)\n", err.message ? err.message : "?");
		dbus_error_free(&err);
	} else {
		dbus_message_unref(reply);
	}

	printf("agent %s registered on %s, capability %s\n", path, adapter, capability);
	fflush(stdout);

	while (dbus_connection_read_write_dispatch(conn, 1000)) {
	}
	return 0;
}

// -------------------------------------------------------------- bluealsa side

// One bluealsa PCM1 object: the transport the daemon holds, with the codec and
// the shape it decodes to. This is what says whether a stream from the phone
// has a place to go, and whether anyone is reading it.
static void print_pcm(const char *path, DBusMessageIter *props, void *user) {
	(void)user;
	printf("%s\n", path);
	DBusMessageIter dict = *props;
	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, value;
		const char *key = NULL;
		dbus_message_iter_recurse(&dict, &entry);
		dbus_message_iter_get_basic(&entry, &key);
		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &value);
		int type = dbus_message_iter_get_arg_type(&value);
		if (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH) {
			const char *s = NULL;
			dbus_message_iter_get_basic(&value, &s);
			printf("    %s = %s\n", key, s ? s : "?");
		} else if (type == DBUS_TYPE_UINT32 || type == DBUS_TYPE_UINT16) {
			dbus_uint32_t v32 = 0;
			dbus_uint16_t v16 = 0;
			if (type == DBUS_TYPE_UINT32) {
				dbus_message_iter_get_basic(&value, &v32);
				printf("    %s = %u\n", key, v32);
			} else {
				dbus_message_iter_get_basic(&value, &v16);
				printf("    %s = %u\n", key, v16);
			}
		} else if (type == DBUS_TYPE_BOOLEAN) {
			dbus_bool_t b = 0;
			dbus_message_iter_get_basic(&value, &b);
			printf("    %s = %s\n", key, b ? "true" : "false");
		} else {
			printf("    %s = <type %d>\n", key, type);
		}
		dbus_message_iter_next(&dict);
	}
}

static int cmd_pcm(const char *mac) {
	if (!mac) {
		fprintf(stderr, "usage: btadm pcm <MAC>\n");
		return 1;
	}
	// bluealsa names its transports after the device and the direction:
	// a2dpsnk/source is the receiver of a phone's stream, a2dpsrc/sink the
	// stream this device sends out.
	const char *dirs[] = {"a2dpsnk/source", "a2dpsrc/sink"};
	char base[256];
	mac_to_path(mac, base, sizeof(base));
	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		char path[320];
		snprintf(path, sizeof(path), "/org/bluealsa/hci0%s/%s", base + strlen("/org/bluez/hci0"), dirs[i]);
		DBusMessage *msg = dbus_message_new_method_call("org.bluealsa", path, "org.freedesktop.DBus.Properties",
														"GetAll");
		const char *iface = "org.bluealsa.PCM1";
		dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID);
		DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 5000, &err);
		dbus_message_unref(msg);
		if (!reply) {
			printf("%s: %s\n", path, err.message ? err.message : "no reply");
			dbus_error_free(&err);
			continue;
		}
		DBusMessageIter it, dict;
		if (dbus_message_iter_init(reply, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
			dbus_message_iter_recurse(&it, &dict);
			printf("%s\n", path);
			print_pcm(path, &dict, NULL);
		}
		dbus_message_unref(reply);
	}
	return 0;
}

// ----------------------------------------------------------------- one device
// A phone exposes its player over AVRCP as org.bluez.MediaPlayer1 on
// /org/bluez/hci0/dev_XX/player0. Play from there is what a pair of
// headphones does when its button is pressed, and it is what makes the phone
// route its audio to the A2DP sink it is paired with: a Play over D-Bus to
// the phone arrives as an AVRCP command over the air.

static void print_player(const char *path, DBusMessageIter *props, void *user) {
	(void)user;
	printf("  %s  status=%s  position=%d\n", path, prop_string(props, "Status") ? prop_string(props, "Status") : "?",
		   0);
}

static int cmd_players(void) {
	DBusMessage *reply = managed_objects(5000);
	if (!reply) {
		fprintf(stderr, "btadm: org.bluez did not answer: %s\n", err.message ? err.message : "?");
		return 1;
	}
	int n = walk_objects(reply, "org.bluez.MediaPlayer1", print_player, NULL);
	dbus_message_unref(reply);
	printf("players: %d\n", n);
	return 0;
}

static int cmd_player(const char *mac, const char *action) {
	const char *method = NULL;
	if (strcmp(action, "play") == 0) {
		method = "Play";
	} else if (strcmp(action, "pause") == 0) {
		method = "Pause";
	} else if (strcmp(action, "stop") == 0) {
		method = "Stop";
	} else if (strcmp(action, "next") == 0) {
		method = "Next";
	} else if (strcmp(action, "prev") == 0) {
		method = "Previous";
	} else {
		fprintf(stderr, "btadm: unknown player action %s\n", action);
		return 1;
	}
	char path[256];
	mac_to_path(mac, path, sizeof(path));
	strncat(path, "/player0", sizeof(path) - strlen(path) - 1);
	return call_method(path, "org.bluez.MediaPlayer1", method, 0, NULL, true);
}

static int cmd_wait(const char *mac, int seconds) {
	const char *path = device_path(mac);
	char last[256] = "";

	for (int i = 0; i < seconds; i++) {
		char now[256] = "not on the bus";
		DBusMessage *reply = managed_objects(3000);
		if (reply) {
			DBusMessageIter root, objects;
			if (dbus_message_iter_init(reply, &root)) {
				dbus_message_iter_recurse(&root, &objects);
				while (dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_DICT_ENTRY) {
					DBusMessageIter entry, interfaces, props;
					const char *p = NULL;
					dbus_message_iter_recurse(&objects, &entry);
					dbus_message_iter_get_basic(&entry, &p);
					dbus_message_iter_next(&entry);
					dbus_message_iter_recurse(&entry, &interfaces);
					if (p && strcmp(p, path) == 0 && iface_props(&interfaces, "org.bluez.Device1", &props)) {
						snprintf(now, sizeof(now), "name=%s paired=%s connected=%s",
								 prop_string(&props, "Alias") ? prop_string(&props, "Alias") : "?",
								 prop_bool(&props, "Paired", false) ? "yes" : "no",
								 prop_bool(&props, "Connected", false) ? "yes" : "no");
					}
					dbus_message_iter_next(&objects);
				}
			}
			dbus_message_unref(reply);
		}
		if (strcmp(now, last) != 0) {
			printf("%s: %s\n", mac, now);
			fflush(stdout);
			snprintf(last, sizeof(last), "%s", now);
		}
		sleep(1);
	}
	return 0;
}

int main(int argc, char **argv) {
	dbus_error_init(&err);
	conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
	if (!conn) {
		fprintf(stderr, "btadm: no system bus: %s\n", err.message ? err.message : "?");
		return 1;
	}
	dbus_connection_set_exit_on_disconnect(conn, false);

	if (argc < 2) {
		fprintf(stderr, "usage: btadm adapters|devices|set|discover|pair|connect|disconnect|trust|remove|wait|agent\n");
		return 1;
	}

	const char *cmd = argv[1];

	if (strcmp(cmd, "adapters") == 0) {
		return cmd_list(false);
	}
	if (strcmp(cmd, "devices") == 0) {
		return cmd_list(true);
	}
	if (strcmp(cmd, "set") == 0) {
		if (argc < 4) {
			fprintf(stderr, "usage: btadm set <property> <value>\n");
			return 1;
		}
		const char *adapter = adapter_path();
		return adapter ? set_property(adapter, "org.bluez.Adapter1", argv[2], argv[3]) : 1;
	}
	if (strcmp(cmd, "discover") == 0) {
		return cmd_discover(argc > 2 ? atoi(argv[2]) : 15);
	}
	if (strcmp(cmd, "agent") == 0) {
		return cmd_agent(argc > 2 ? argv[2] : agent_capability);
	}
	if (strcmp(cmd, "players") == 0) {
		return cmd_players();
	}
	if (strcmp(cmd, "pcm") == 0) {
		return cmd_pcm(argc > 2 ? argv[2] : NULL);
	}
	if (strcmp(cmd, "player") == 0) {
		if (argc < 4) {
			fprintf(stderr, "usage: btadm player <MAC> play|pause|stop|next|prev\n");
			return 1;
		}
		return cmd_player(argv[2], argv[3]);
	}
	if (strcmp(cmd, "wait") == 0) {
		if (argc < 3) {
			fprintf(stderr, "usage: btadm wait <MAC> [seconds]\n");
			return 1;
		}
		return cmd_wait(argv[2], argc > 3 ? atoi(argv[3]) : 60);
	}
	if (strcmp(cmd, "pair") == 0 || strcmp(cmd, "connect") == 0 || strcmp(cmd, "disconnect") == 0) {
		if (argc < 3) {
			fprintf(stderr, "usage: btadm %s <MAC>\n", cmd);
			return 1;
		}
		const char *path = device_path(argv[2]);
		const char *method = strcmp(cmd, "pair") == 0 ? "Pair" : (strcmp(cmd, "connect") == 0 ? "Connect" : "Disconnect");
		return call_method(path, "org.bluez.Device1", method, 0, NULL, true);
	}
	if (strcmp(cmd, "remove") == 0) {
		if (argc < 3) {
			fprintf(stderr, "usage: btadm remove <MAC>\n");
			return 1;
		}
		const char *adapter = adapter_path();
		char *args[1] = {(char *)device_path(argv[2])};
		if (!adapter) {
			return 1;
		}
		return call_method(adapter, "org.bluez.Adapter1", "RemoveDevice", 1, args, true);
	}
	if (strcmp(cmd, "trust") == 0) {
		if (argc < 4) {
			fprintf(stderr, "usage: btadm trust <MAC> <yes|no>\n");
			return 1;
		}
		const char *path = device_path(argv[2]);
		return set_property(path, "org.bluez.Device1", "Trusted", argv[3]);
	}

	fprintf(stderr, "btadm: unknown command %s\n", cmd);
	return 1;
}
