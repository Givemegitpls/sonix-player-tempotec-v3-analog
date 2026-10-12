// btprobe -- what the system bus says about Bluetooth, without bluetoothctl.
//
// The TempoTec V3 Analog's firmware carries BlueZ 4.101, whose D-Bus interface
// (org.bluez.Adapter, org.bluez.Manager) is not the one this port and bluealsa
// speak. When a BlueZ 5 bluetoothd is put in its place, this is what says
// whether it came up and whether it found the controller:
//
//   btprobe          adapters, their state, and whether bluealsa is on the bus
//   btprobe -d       the same, plus every device the adapter knows
//
// It is one GetManagedObjects call on org.bluez (the BlueZ 5 ObjectManager
// layout: /org/bluez/hci0 and /org/bluez/hci0/dev_XX_...), so it needs nothing
// but libdbus -- no readline, no bluetoothctl, no tools at all.
//
// Build (against the device's libdbus, from this directory):
//   rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-gcc -O2 -o btprobe btprobe.c \
//       -I<sysroot>/usr/include/dbus-1.0 -I<sysroot>/usr/lib/mipsel-linux-gnu/dbus-1.0/include \
//       -L<sysroot>/usr/lib -ldbus-1

#include <dbus/dbus.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static bool show_devices;

// The value of one string property, or NULL. The returned pointer belongs to
// the iterator and stays valid until it is consumed.
static const char *prop_string(DBusMessageIter *props, const char *name) {
	DBusMessageIter dict = *props;
	static char buf[256];

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
				snprintf(buf, sizeof(buf), "%s", s ? s : "");
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

static bool object_has_iface(DBusMessageIter *interfaces, const char *iface) {
	DBusMessageIter it = *interfaces;

	while (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry;
		const char *key = NULL;

		dbus_message_iter_recurse(&it, &entry);
		dbus_message_iter_get_basic(&entry, &key);
		if (key && strcmp(key, iface) == 0) {
			return true;
		}
		dbus_message_iter_next(&it);
	}
	return false;
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

int main(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-d") == 0) {
			show_devices = true;
		}
	}

	DBusError err;
	dbus_error_init(&err);

	DBusConnection *conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
	if (!conn) {
		fprintf(stderr, "btprobe: no system bus: %s\n", err.message ? err.message : "?");
		dbus_error_free(&err);
		return 1;
	}

	// The name of whoever owns org.bluez, and whether bluealsa is there: both
	// are what the port's own code looks for.
	const char *bluez_owner = dbus_bus_get_unique_name(conn) ? NULL : NULL;
	(void)bluez_owner;

	DBusMessage *msg = dbus_message_new_method_call("org.bluez", "/", "org.freedesktop.DBus.ObjectManager",
													"GetManagedObjects");
	if (!msg) {
		fprintf(stderr, "btprobe: out of memory\n");
		return 1;
	}

	DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 5000, &err);
	dbus_message_unref(msg);
	if (!reply) {
		fprintf(stderr, "btprobe: org.bluez did not answer: %s\n", err.message ? err.message : "?");
		dbus_error_free(&err);
		return 1;
	}

	DBusMessageIter root;
	if (!dbus_message_iter_init(reply, &root) || dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_ARRAY) {
		fprintf(stderr, "btprobe: unexpected reply shape\n");
		return 1;
	}

	int adapters = 0, devices = 0;
	DBusMessageIter objects = root;
	dbus_message_iter_recurse(&root, &objects);

	while (dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, interfaces;
		const char *path = NULL;

		dbus_message_iter_recurse(&objects, &entry);
		dbus_message_iter_get_basic(&entry, &path);
		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &interfaces);

		DBusMessageIter props;
		if (iface_props(&interfaces, "org.bluez.Adapter1", &props)) {
			adapters++;
			const char *addr = prop_string(&props, "Address");
			const char *name = prop_string(&props, "Alias");
			printf("adapter %s: %s  name=%s  powered=%s  discoverable=%s\n", path, addr ? addr : "?",
				   name ? name : "?", prop_bool(&props, "Powered", false) ? "yes" : "no",
				   prop_bool(&props, "Discoverable", false) ? "yes" : "no");
		} else if (show_devices && iface_props(&interfaces, "org.bluez.Device1", &props)) {
			devices++;
			const char *addr = prop_string(&props, "Address");
			const char *name = prop_string(&props, "Alias");
			printf("  device %s: %s  name=%s  connected=%s  paired=%s\n", path, addr ? addr : "?",
				   name ? name : "?", prop_bool(&props, "Connected", false) ? "yes" : "no",
				   prop_bool(&props, "Paired", false) ? "yes" : "no");
		}

		dbus_message_iter_next(&objects);
	}

	dbus_message_unref(reply);
	printf("adapters: %d", adapters);
	if (show_devices) {
		printf(", devices: %d", devices);
	}
	printf("\n");

	// bluealsa is a separate name on the same bus; its absence is why a port
	// with everything else in place still has no sound over Bluetooth.
	DBusMessage *has = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
													"org.freedesktop.DBus", "NameHasOwner");
	const char *names[] = {"org.bluealsa", "org.bluez"};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		DBusMessage *m = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
													  "org.freedesktop.DBus", "NameHasOwner");
		dbus_message_append_args(m, DBUS_TYPE_STRING, &names[i], DBUS_TYPE_INVALID);
		DBusMessage *r = dbus_connection_send_with_reply_and_block(conn, m, 3000, NULL);
		dbus_bool_t owned = 0;
		if (r) {
			dbus_message_get_args(r, NULL, DBUS_TYPE_BOOLEAN, &owned, DBUS_TYPE_INVALID);
			dbus_message_unref(r);
		}
		printf("%s: %s\n", names[i], owned ? "on the bus" : "NOT on the bus");
		dbus_message_unref(m);
	}
	dbus_message_unref(has);

	return adapters > 0 ? 0 : 2;
}
