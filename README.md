# Corsair Harpoon Wireless LED off helper

This dependency-free Linux helper turns off both RGB zones on the wired USB
identity (`1b1c:1b5e`) of the Corsair Harpoon RGB Wireless mouse. It sends only
the small subset of Corsair's Bragi HID protocol needed to enter software mode
and write black to both zones.

A udev rule cannot turn off this LED by assigning a sysfs attribute: the mouse
requires a sequence of vendor HID reports. The included rule selects USB
interface 1 (HID usage `ff42:0001`) and starts a systemd service. The helper
reasserts the setting every 45 seconds so resets and software-mode timeouts do
not bring the lighting back.

## Install on Arch Linux

Build and install:

```sh
make
sudo make install
sudo systemctl daemon-reload
sudo udevadm control --reload-rules
sudo systemctl enable --now corsair-harpoon-led-off-trigger.service
```

The enabled oneshot replays hidraw events late in each boot. This is necessary
when the mouse is discovered in the initramfs before the installed udev rule and
system service are available. It also handles the already-present interface at
installation time. Alternatively, unplug and reconnect the mouse or trigger it
manually:

```sh
sudo udevadm trigger --action=add --subsystem-match=hidraw
```

Check the result (replace `hidrawN` with the interface selected by the rule):

```sh
systemctl status 'corsair-harpoon-led-off@*'
journalctl -u 'corsair-harpoon-led-off@*'
```

## Persistent pointer speed under Xorg

Installation also adds an Xorg `InputClass` configuration that replaces these
per-session `xinput` settings:

- Direct Harpoon Gaming Mouse: `AccelSpeed` `-0.9` (manually tested)
- Harpoon Wireless USB Receiver: `AccelSpeed` `-0.8`
- Slipstream Wireless USB Receiver: `AccelSpeed` `-0.8`

All three entries explicitly keep `TransformationMatrix` at identity,
`1 0 0 0 1 0 0 0 1`. The previous 2× transform reproducibly breaks
Chromium/X11 pointer lock; do not use it for sensitivity.

The wired mouse was manually tuned from `-1.0` to `-0.9` with an identity
matrix and felt acceptable. Keep hardware DPI at the value used during that
trial and leave the acceleration profile unchanged; do not also double DPI.
The receiver speeds remain unchanged and have not been retuned.

Libinput speed ranges from `-1.0` (minimum, not neutral) to `1.0`. There is no
universal value equivalent to the old post-acceleration 2× transform.
Hardware DPI is an alternative if supported, not an additional adjustment
to stack on this tuned setting. This helper does not read, set, or persist
DPI, and DPI/profile support on the connected mouse has not been verified.
Remove any per-session `xinput` or desktop autostart overrides that would
overwrite these settings.

To update only the pointer configuration without touching the LED service:

```sh
sudo install -Dm644 90-corsair-harpoon-pointer.conf \
    /etc/X11/xorg.conf.d/90-corsair-harpoon-pointer.conf
```

Xorg applies the setting whenever a matching pointer appears. Restart the Xorg
session after installation; subsequent reboots and device reconnections do not
require an `xinput` script. Restarting the LED service does not apply pointer
settings. Verify the active values with:

```sh
xinput list-props "Corsair CORSAIR HARPOON RGB WIRELESS Gaming Mouse" \
    | grep -E 'libinput Accel Speed|Coordinate Transformation Matrix'
```

For the direct mouse, expect speed `-0.900000`
and matrix `1, 0, 0, 0, 1, 0, 0, 0, 1` (displayed with decimal places).

For a live adjustment before restarting Xorg, use the active pointer's name
or device ID from `xinput list` (the example below is the direct/wired mouse):

```sh
xinput set-prop "Corsair CORSAIR HARPOON RGB WIRELESS Gaming Mouse" \
    "Coordinate Transformation Matrix" 1 0 0 0 1 0 0 0 1
```

Apply the tuned speed with DPI left at the value used during the trial:

```sh
xinput set-prop "Corsair CORSAIR HARPOON RGB WIRELESS Gaming Mouse" \
    "libinput Accel Speed" -0.9
```

These live changes are temporary; persist the chosen speed in the Xorg file
and install it using the command above. Log out of the Xorg session and back
in to apply the file to a new X server (a reboot also works). No systemd
service restart, `daemon-reload`, udev reload, helper rebuild, or full
`make install` is needed for this pointer-only change. Leave the running
LED-off service alone. If a separate vendor configuration tool is needed
for DPI, do not run it concurrently with the LED helper; stop the active
`corsair-harpoon-led-off@hidrawN.service` instance first, close the vendor
tool afterward, then start that same instance again.

The configuration is specific to Xorg. A native Wayland session would require
the equivalent setting in its compositor instead.

For a one-time manual test, find the Harpoon entry whose USB interface number is
`01`, then run the helper as root:

```sh
for d in /sys/class/hidraw/hidraw*; do
    udevadm info -q property -p "$d" | grep -q '^ID_USB_INTERFACE_NUM=01$' || continue
    udevadm info -q property -p "$d" | grep -q '^ID_VENDOR_ID=1b1c$' || continue
    udevadm info -q property -p "$d" | grep -q '^ID_MODEL_ID=1b5e$' || continue
    echo "/dev/${d##*/}"
done
sudo ./corsair-harpoon-led-off /dev/hidrawN
```

Stop the manual test with Ctrl-C. Do not run this helper concurrently with
OpenRGB or ckb-next; they use the same vendor command channel.

## Scope and limitations

- Normal pointer/buttons use interface 0 and are not touched.
- This intentionally supports only direct/wired PID `1b5e`, which is the device
  observed while developing it.
- Wireless receiver mode uses a different transport and is not supported. In
  particular, the Harpoon dongle PID `1b65` must not be treated as the mouse.
- This changes live software lighting, not the mouse's stored hardware profile.
  The daemon is therefore kept running for reliability.

Uninstall with:

```sh
sudo systemctl disable --now corsair-harpoon-led-off-trigger.service
sudo make uninstall
sudo systemctl daemon-reload
sudo udevadm control --reload-rules
```

The report sequence and two-zone channel layout are derived from the OpenRGB
`CorsairPeripheralV2Controller` and ckb-next Bragi implementations.
