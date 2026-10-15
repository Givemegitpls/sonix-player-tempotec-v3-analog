#ifndef BOARD_H
#define BOARD_H

#include "sysinfo.h"

// The one board this firmware is built for.
//
// There is no table of players here and nothing is chosen at run time. The
// device this player runs on is decided by BOARD in the Makefile, and what
// follows from it -- the panel the interface is laid out for, the icon set that
// is compiled in, the mixer card, the PCM, the backlight node, the key device,
// the USB gadget, the Bluetooth stack, whether the panel may be blanked -- is a
// constant of the build. The values for the boards this binary is not for are
// not in it, and every branch that would have picked between them is folded
// away by the compiler.
//
// What the device's own system-info.json still says is the version numbers the
// About page prints, and the name, which is compared against the one below: a
// binary built for another board would lay the interface out for the wrong
// glass, so a mismatch is worth a loud line in the log.
//
// Adding a board means adding a branch here (or a file beside this one) with
// its own values, and its own BOARD entry in the Makefile.

#if defined(SONIX_BOARD_TEMPOTEC_V3)

// TempoTec Variations V3 Analog. Everything below was measured on the device
// rather than taken from the specification, which is wrong in the details (it
// claims a 480x720 glass, 64 MB of RAM and PCM to 768 kHz).
static const sysinfo_model_t BOARD_MODEL = {
	.name = "TempoTec V3 Analog",
	// The glass itself, not a larger layout: this board is the one that draws at
	// the panel's own size. 320x480 is what the kernel reports for fb0 and what
	// the stock firmware writes. The interface is still written at the older
	// 480x720 numbers -- every absolute size and position goes through SC()
	// (src/gui/shell/scale.h), which is two thirds of them for this board, at
	// compile time.
	.panel_width = 320,
	.panel_height = 480,
	.update_stem = "v3_analog",
	.serial_prefix = "V3",
	// The stock player's own node for the number printed on the box (its About
	// page reads the same file), and the box's number starts with the model's
	// own "V3". The HiBy boards' efuse_chip_id holds a different number, which
	// is why the prefix and the node travel together here.
	.serial_node = "/proc/jz/efuse/efuse_serial_nb",
	// The node holds the number itself, with no chip-id line around it, so the
	// whole line is the serial's own part (the box adds "V3" in front).
	.serial_digits = 0,
	// The box's number carries the check digit the stock player computes, and on
	// this unit the computed digit matches the box exactly.
	.serial_check_digit = true,
	// Charged through /sys/class/axppower and /sys/class/power_supply/usb rather
	// than the AXP2101 over I2C: there is no /dev/i2c-0 on this board. Until that
	// path is written the charge limit is off rather than writing nothing and
	// saying it did.
	.pmic_charger = false,
	.cs43131 = false,
	.ak4493 = true,
	.remap_list = true,
	.tap_wake = false,
	.one_flank = false,
	// The key the kernel calls NEXTSONG is the one the case prints as back, as
	// on the R3 Pro II. Said by the user watching the list move the wrong way
	// when the forward button was pressed.
	.media_keys_swapped = true,
	// No /etc/logo.jpeg here (TempoTec's own logo is on the Blaze only), and
	// /dev/mtd5 is the /data UBI volume rather than a scratch partition, so the
	// theme marker must never be written to flash.
	.one_boot_logo = false,
	.boot_logo_flash = false,
	// The kernel driver registers "pwm-backlight.0" here, and the stock firmware
	// writes the same node. The HiBy boards' "backlight_pwm0" does not exist on
	// this one -- writes to it go nowhere and the screen stays at whatever level
	// the last process left it at.
	.backlight_dir = "/sys/class/backlight/pwm-backlight.0",
	// card0 on this board is a dummy SPDIF device; the AK4493 mixer is card1.
	.mixer_card = 1,
	// The writable volume, mounted at /data (ubi1_0). The Blaze's /usr/data does
	// not exist here, and a settings file that cannot be written is a player
	// that forgets the language, the time zone and the volume on every boot.
	.user_data_dir = "/data",
	// The kernel registers the case keys as "gpio-keys"; the family names have
	// never matched it, so a build watching those names saw no key device at all
	// and the volume and skip keys did nothing.
	.buttons_device_name = "gpio-keys",
	// The stock firmware's own bluetoothd path (its init starts that one) and
	// its own firmware directory: the .hcd sits in /lib/firmware, not in a
	// bt_bcm subdirectory.
	.bluetoothd_path = "/usr/sbin/bluetoothd",
	.bt_firmware_dir = "/lib/firmware",
	// The controller's UART attach belongs to the firmware's own /usr/bin/bt_init
	// at boot. Doing it a second time from here -- or killing the one that is
	// running -- leaves the controller with no index at all, which is a radio
	// bluez cannot see.
	.bt_chip_by_firmware = true,
	// DAC mode goes through the firmware's legacy android_usb gadget: its uac_sa
	// function is one of the modes there, with these ids and this function list
	// (the stock player's own values). The configfs gadget the other boards use
	// is not available on this kernel.
	.usb_gadget_legacy = true,
	.usb_audio_vendor = "22E1",
	.usb_audio_product = "F008",
	.usb_audio_functions = "uac_sa,hid",
	// The port manages the Bluetooth stack here: BlueZ 5 and bluealsa built for
	// this glibc, started by the player (see NOTES.md).
	.bluetooth_stack = true,
	// One framebuffer page, and the copy waits for the panel to have taken the
	// frame before it writes (see panel_wait_transfer in main.c).
	//
	// Two pages were tried, and the measurement says they are the wrong trade
	// here. What they buy is real: the copy goes into the page the panel is not
	// showing, so the wait for the panel's own transfer (7 to 13 ms of every
	// frame -- the transfer reads the framebuffer page for 12.3 ms of the panel's
	// 13.3 ms frame) disappears from the frame budget and forty frames a second
	// fit again. What they cost is the frame on the glass being one frame old,
	// and that is what the hand notices: the owner drove both builds and said the
	// two-page one felt less smooth, whatever the counter said. Latency in the
	// finger's feedback is worth more than frames in the counter.
	//
	// So: one page, the wait, and thirty frames a second as the rate that leaves
	// the frame its slack. The two-page path is still in main.c (rot_page_pending
	// keeps what each page owes the other) and the image with it is kept on the
	// card as v3_analog_2page.upt, should a faster glass ever matter more than
	// the delay.
	.single_fb_page = true,
	// The interface is drawn at the panel's own 320x480, in the panel's own
	// pixel format: LVGL renders RGB565 into a page of its own and one pass per
	// frame expands it into the framebuffer the panel scans. Drawing straight
	// into the framebuffer was tried and is wrong here (32-bit blending with
	// alpha costs more than the pixels saved, and tiles appear on the glass as
	// they are written -- see main.c and NOTES.md).
	.native_panel = true,
	// The panel is never blanked through the framebuffer node. The stock player
	// on this device does not write one at all -- its binary has no FBIOBLANK,
	// no FBIOPAN_DISPLAY and no FBIOPUT_VSCREENINFO in it: it dims the backlight
	// and leaves the panel scanning its last frame. The driver's blank drops
	// LCDC_CTRL.ENA and SLCDC_CTRL.DMA_EN, and the unblank brings back only the
	// first, so a blanked-then-unblanked panel can sit armed with DMA_EN=1 and
	// show nothing at all -- the frozen picture this flag exists to keep out.
	.blank_panel = false,
	// plughw and not hw: the codec's PCM accepts a narrow set of formats and
	// rates, and what it takes exactly has not been measured yet, so the plug is
	// what keeps a 24/96 track from being refused outright. The name "default"
	// on this board is the dummy SPDIF device, not the analog output.
	.pcm_device = "plughw:1,0",
};

// What the packer writes into system-info.json and what sysinfo_load() compares
// the file's own device-name against.
#define SONIX_BOARD_NAME "TempoTec V3 Analog"

#else
#error "no board selected. Build with BOARD=tempotec_v3 (see the Makefile): this firmware is built for one piece of hardware, and there is no universal binary."
#endif

#endif // BOARD_H
