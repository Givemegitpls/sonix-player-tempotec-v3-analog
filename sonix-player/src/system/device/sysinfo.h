#ifndef SYSINFO_H
#define SYSINFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// What the information page has to show: the two version numbers, and how much
// space is left on the card.

// ---------------------------------------------------------------------------
// versions
// ---------------------------------------------------------------------------
//
// Not compiled into the binary but read from /usr/resource/sonix/components/system-info.json, a
// text file next to the language directory:
//
//     {
//         "device-name": "HiBy R3 Pro II",
//         "dac-info": "Dual Cirrus Logic CS43198",
//         "OS_version": "1.0",
//         "Build_version": "182",
//         "ota-repo": "Jepl4r/sonix-player"
//     }
//
// This way updating the build number does not mean rebuilding, and whoever
// assembles a firmware image can write it from the build script. A missing or
// unreadable file is not an error: the entries show a dash.
//
// "device-name" is what tells one player from another. The firmware packer
// writes a different one into each image, and it is the whole of what this
// binary knows about which machine it woke up on -- see sysinfo_model().

// Rereads the file. Called once at startup; the functions below do not touch
// the disk.
void sysinfo_load(void);

// "" when the file is missing or does not carry the key.
const char *sysinfo_os_version(void);
const char *sysinfo_build_version(void);

// The name of the player, verbatim from the file: "HiBy R3 Pro II", "HiBy R1".
// "" when the file does not say.
const char *sysinfo_device_name(void);

// The converter, verbatim from the file, for the information page to print.
// The player never decides anything by it.
const char *sysinfo_dac_info(void);

// The GitHub repository the update from the internet looks in, "owner/name",
// verbatim from the file (ota.h). "" when the file does not say.
const char *sysinfo_ota_repo(void);

// ---------------------------------------------------------------------------
// the models, and what differs between them
// ---------------------------------------------------------------------------
//
// One table, keyed on device-name.
//
// The panel is written here rather than read from the framebuffer: the pages
// subtract from screen_height in unsigned arithmetic, and LVGL's fbdev driver
// reports 800x480 until its file is set. The framebuffer size only picks a row
// of this table, by exact match, when the name is missing (panel_size() in
// main.c).
typedef struct {
	const char *name;		 // as written in system-info.json
	int panel_width;		 // the panel, on the device and on the simulator alike
	int panel_height;
	const char *update_stem; // "<stem>.upt" is the update file this player accepts
	const char *serial_prefix; // what the number on the box starts with
	const char *serial_node;   // the efuse node the box's number is read from; NULL is the HiBy
							   // boards' own "/proc/jz/efuse/efuse_chip_id", whose line reads
							   // "CHIP_ID: <32 hex>". The V3 Analog has no such node: its serial
							   // number sits in "/proc/jz/efuse/efuse_serial_nb", which is what
							   // the stock player reads for its About page.
	int serial_digits;		   // how many leading characters of the node make the number: 8 on
							   // the HiBy boards (the first eight hex of the chip id, which is
							   // what their box shows). 0 means the node's whole first line is
							   // the number, which is the shape the V3 Analog's serial node has.
	bool serial_check_digit;   // append the EAN/UPC-style check digit the stock player computes:
							   // base-36 value of every character of prefix+serial, weight 3 on
							   // every other one counting from the right, and (10 - sum%10) % 10 as a
							   // decimal digit. Set where the result was checked against the box
							   // (the V3 Analog); the stock applies it on every model, but the
							   // other models' About pages are not ours to change unverified.
	bool pmic_charger;		   // charged by the AXP2101 alone, no MP2731 (axpcharge.h)
	bool cs43131;			   // one CS43131, 3.5 mm only, no HBC3000 (alsa-controls.h)
	bool ak4493;			   // dual AK4493 behind the HBC3000: six filters, no DRE/NOS (alsa-controls.h)
	bool remap_list;		   // no photos of this player: the remap page is a plain list (remap.c)
	bool tap_wake;			   // the touch controller can wake the screen (power.h)
	bool one_flank;			   // every button on the right flank (remap.c)
	bool media_keys_swapped;   // KEY_NEXTSONG is the upper skip key, not next (system.c)
	bool one_boot_logo;		   // /etc/logo.jpeg whatever the theme; logo1/logo2 are not its own (bootlogo.h)
	bool boot_logo_flash;	   // the theme marker is written into the flash (bootlogo.c). False where the
							   // partition that holds it is a filesystem in use, as on the V3 Analog, where
							   // /dev/mtd5 is the UBI volume /data is mounted from.
	const char *backlight_dir; // /sys/class/backlight/<this>, whose brightness and max_brightness the
							   // player writes (main.c). The name belongs to the kernel driver, not to
							   // the player: NULL keeps the HiBy boards' own "backlight_pwm0", and a
							   // wrong one here is silent -- the writes go nowhere and the screen stays
							   // at the level the last process left it at.
	bool bluetooth_stack;	   // the Bluetooth stack this port manages is the one that runs here
							   // (main.c). False where the firmware's own stack is a different
							   // one -- the V3 Analog's -- so nothing talks to it, and nothing
							   // retries a registration against it every three seconds either.
	bool single_fb_page;	   // drive the panel from one framebuffer page instead of two (main.c).
							   // The V3 Analog's panel is an SLCD whose DMA reads one page; with two the
							   // frame the user is looking at is always the one written two frames ago,
							   // and each page owes the other everything that changed while it was off
							   // screen, which is a full-screen copy per frame. One page and no debt.
	bool native_panel;		   // LVGL draws the interface at the panel's own size, in the panel's own
							   // pixel format, straight into the framebuffer (main.c). False keeps the
							   // older path: the interface is laid out at panel_width x panel_height
							   // (which is then larger than the glass) and shrunk onto it on the way
							   // out, one 2/3 copy of every changed rectangle per frame.
							   //
							   // True only where panel_width/panel_height really are the glass, and
							   // where the glass scans out 32 bits a pixel in BGRA order -- LVGL's
							   // ARGB8888 in memory on a little-endian machine. On such a panel drawing
							   // at the panel's size removes the copy altogether: LVGL blends in the
							   // format the panel scans, and every pixel lands where the panel reads
							   // it. The layout then has to be authored for that size too, which is
							   // what SC() in src/gui/shell/scale.h is for.
	bool blank_panel;		   // screen-off writes the framebuffer blank node (power.h, power.c).
							   // False where that node is not the way down -- not because the panel
							   // stays powered: on the V3 Analog it is powered down with the vendor
							   // SET_SLEEP ioctl below, which is what its stock player does. FBIOBLANK
							   // is the path that wedges this glass (LCDC_CTRL.ENA comes back on
							   // unblank, SLCDC_CTRL.DMA_EN does not, and a panel in that state shows
							   // nothing until something power-cycles it), so the two fields are
							   // exclusive on this model.
	// The vendor ioctl that powers this panel down (jzfb SET_SLEEP), 0 where
	// there is none. The stock player on the V3 Analog uses it on every
	// screen-off: the kernel's jzfb_suspend waits for the SLCDC transfer,
	// clears LCDC_CTRL.ENA, gates the clocks, drops the AXP2101 rails
	// (LDO7/LDO8) and the reset GPIO, then the same ioctl with 0 puts it all
	// back (see docs/PANEL-POWER.md). Distinct from blank_panel: FBIOBLANK is
	// the path that wedges this glass, this one is the path the stock
	// firmware itself takes.
	uint32_t panel_sleep_ioctl; // 0 = the panel is never powered down on screen-off
	int mixer_card;			   // the ALSA card whose mixer controls this board's codec (alsa-controls.c):
							   // 0 on the HiBy boards and the Blaze, 1 on the V3 Analog
	const char *user_data_dir; // where the settings and the reader's file are written; NULL is
							   // "/usr/data" (sysinfo_user_data_dir)
	const char *bluetoothd_path; // the BlueZ daemon this port starts, and where the controller's
							   // firmware file lives (bluetooth.c). NULL keeps the HiBy boards'
	const char *bt_firmware_dir; // own "/usr/libexec/bluetooth/bluetoothd" and "/lib/firmware/bt_bcm";
							   // the V3 Analog's are "/usr/sbin/bluetoothd" and "/lib/firmware".

	// USB audio ("DAC mode"). The HiBy boards and the Blaze build a configfs
	// gadget and create a UAC function in it; the V3 Analog's firmware has a
	// legacy android_usb gadget instead, one of whose modes carries the same UAC
	// function -- the stock player switches to it by writing these ids and the
	// function list (usbdac.c).
	bool usb_gadget_legacy;
	// The controller's transport (the UART attach that loads the chip's firmware)
	// belongs to the firmware on this board: its own /usr/bin/bt_init does it at
	// boot, and doing it a second time from here -- or killing the one that is
	// running -- leaves the controller with no index at all, which is a radio
	// that bluez cannot see. True means: leave it alone, and ask the firmware to
	// attach it when it is not up.
	bool bt_chip_by_firmware;
	const char *usb_audio_vendor;	 // 22E1 on the V3 Analog, as the stock writes it
	const char *usb_audio_product;	 // F008
	const char *usb_audio_functions; // uac_sa,hid -- the stock's own list
	const char *buttons_device_name; // the input device the case's keys arrive from, by the name the
							   // kernel gives it (system.c). NULL is the HiBy boards' own pair,
							   // "md-gpio-keys"/"jz adc keyboard"; the V3 Analog's is "gpio-keys",
							   // which neither list holds -- and a key device nothing reads is a
							   // player whose volume and skip buttons do nothing at all
	const char *pcm_device;	   // the ALSA name the built-in analogue output is opened as; NULL is "default"
	const char *digital_pcm;   // the ALSA name of a second, separately opened output -- the S/PDIF one on
							   // the V3 Analog, where the optical route is card 0 and the analogue one is
							   // card 1. NULL means the board has one output and the route only touches the
							   // mixer, as on the HiBy boards and the Blaze
} sysinfo_model_t;

// The writable user area: /usr/data on the HiBy boards and the Blaze, /data on
// the V3 Analog (a UBIFS volume of its own, mounted, empty and writable). The
// settings and the reader's file live here; sysinfo_user_data_dir() is the
// resolved path, "/usr/data" when the model does not name one.
const char *sysinfo_user_data_dir(void);

// The entry for the name in the file, or NULL when the file says nothing or
// names a player this binary does not know. NULL is not a failure to paper
// over: see firmware_update_file_find().
const sysinfo_model_t *sysinfo_model(void);

// The entry whose panel is exactly width x height, or NULL. For sizing the
// interface when system-info.json names no model; not for choosing an update
// file or a serial prefix, which need the name itself.
const sysinfo_model_t *sysinfo_model_by_panel(int width, int height);

// The device serial number: the model's prefix plus the first eight hex digits
// (upper case) of the SoC efuse chip id (/proc/jz/efuse/efuse_chip_id, line
// "CHIP_ID: <32 hex>"). It is exactly the number printed on the box of either
// model (chip id 90a70a42... -> box R3PII90A70A42). "" when the node
// is absent (the host build, or a firmware without the efuse module) or the
// model is unknown. Read once and kept: efuses do not change.
const char *sysinfo_serial_number(void);

// ---------------------------------------------------------------------------
// microSD card
// ---------------------------------------------------------------------------

typedef struct {
	bool present;	 // false when no card is mounted
	uint64_t total;	 // bytes
	uint64_t used;	 // bytes
	uint64_t free;	 // bytes available to this process, not counting reserved
	int used_percent; // 0..100, rounded
} sysinfo_storage_t;

// false when there is no card or it cannot be queried.
bool sysinfo_sd_usage(sysinfo_storage_t *out);

// "12.3 GB", "512 MB", "48 KB". Decimal point rather than comma: it is the same
// string in all five languages and does not pass through tr().
void sysinfo_format_size(uint64_t bytes, char *out, size_t out_size);

#endif /* SYSINFO_H */
