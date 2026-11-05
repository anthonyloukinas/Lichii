"""Virtual Xbox-style gamepad on /dev/uinput (python3-evdev).

SDL3's Linux evdev backend numbers joystick buttons in key-code order from BTN_JOYSTICK upward
(BTN_SOUTH..BTN_THUMBR here) and axes in ABS code order, hats separately. With the capability set
below that gives exactly the indices the harness writes into qt-config.ini:

    button 0 SOUTH  1 EAST  2 NORTH  3 WEST  4 TL  5 TR  6 SELECT  7 START  8 MODE  9 THUMBL  10 THUMBR
    axis   0 X  1 Y  2 Z(LT)  3 RX  4 RY  5 RZ(RT)      hat 0 = HAT0X/HAT0Y

Switch mapping used by the harness (Nintendo positions): A=EAST(1) B=SOUTH(0) X=WEST(3) Y=NORTH(2),
L=TL R=TR ZL=Z ZR=RZ, Plus=START Minus=SELECT Home=MODE.
"""
import time

from evdev import AbsInfo, UInput, ecodes as e

VENDOR, PRODUCT, VERSION, BUS = 0x1209, 0x0e0d, 0x0001, e.BUS_USB
NAME = "OOT Harness Pad"

BUTTONS = [e.BTN_SOUTH, e.BTN_EAST, e.BTN_NORTH, e.BTN_WEST, e.BTN_TL, e.BTN_TR,
           e.BTN_SELECT, e.BTN_START, e.BTN_MODE, e.BTN_THUMBL, e.BTN_THUMBR]
# Switch button name -> evdev code
SWITCH = {"a": e.BTN_EAST, "b": e.BTN_SOUTH, "x": e.BTN_WEST, "y": e.BTN_NORTH,
          "l": e.BTN_TL, "r": e.BTN_TR, "plus": e.BTN_START, "minus": e.BTN_SELECT,
          "home": e.BTN_MODE, "lstick": e.BTN_THUMBL, "rstick": e.BTN_THUMBR}
STICK = AbsInfo(value=0, min=-32768, max=32767, fuzz=16, flat=128, resolution=0)
TRIG = AbsInfo(value=0, min=0, max=1023, fuzz=0, flat=0, resolution=0)
HAT = AbsInfo(value=0, min=-1, max=1, fuzz=0, flat=0, resolution=0)


def sdl_guid():
    """The GUID string Eden stores (SDL3 Linux GUID with the name-CRC bytes cleared)."""
    le = lambda v: bytes([v & 0xff, v >> 8])
    raw = le(BUS) + b"\0\0" + le(VENDOR) + b"\0\0" + le(PRODUCT) + b"\0\0" + le(VERSION) + b"\0\0"
    return raw.hex()


class Pad:
    def __init__(self):
        caps = {e.EV_KEY: BUTTONS,
                e.EV_ABS: [(e.ABS_X, STICK), (e.ABS_Y, STICK), (e.ABS_Z, TRIG),
                           (e.ABS_RX, STICK), (e.ABS_RY, STICK), (e.ABS_RZ, TRIG),
                           (e.ABS_HAT0X, HAT), (e.ABS_HAT0Y, HAT)]}
        self.ui = UInput(caps, name=NAME, vendor=VENDOR, product=PRODUCT, version=VERSION, bustype=BUS)
        self.devnode = self.ui.device.path if self.ui.device else None

    def close(self):
        try:
            self.neutral()
        finally:
            self.ui.close()

    def _syn(self):
        self.ui.syn()

    def neutral(self):
        for code in (e.ABS_X, e.ABS_Y, e.ABS_RX, e.ABS_RY, e.ABS_Z, e.ABS_RZ, e.ABS_HAT0X, e.ABS_HAT0Y):
            self.ui.write(e.EV_ABS, code, 0)
        for b in BUTTONS:
            self.ui.write(e.EV_KEY, b, 0)
        self._syn()

    def press(self, name, hold=0.12):
        code = SWITCH[name]
        self.ui.write(e.EV_KEY, code, 1); self._syn()
        time.sleep(hold)
        self.ui.write(e.EV_KEY, code, 0); self._syn()

    def stick(self, which, x, y):
        """which='l'|'r'; x,y in [-1,1]; y=-1 is up/forward (evdev convention)."""
        ax, ay = (e.ABS_X, e.ABS_Y) if which == "l" else (e.ABS_RX, e.ABS_RY)
        clamp = lambda v: max(-32768, min(32767, int(round(v * 32767))))
        self.ui.write(e.EV_ABS, ax, clamp(x)); self.ui.write(e.EV_ABS, ay, clamp(y)); self._syn()

    def hold_stick(self, which, x, y, seconds, refresh=0.05):
        """Hold a stick deflection, re-sending periodically so SDL never sees a stale state."""
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.stick(which, x, y)
            time.sleep(refresh)
        self.stick(which, 0, 0)

    def dpad(self, x, y, hold=0.12):
        self.ui.write(e.EV_ABS, e.ABS_HAT0X, x); self.ui.write(e.EV_ABS, e.ABS_HAT0Y, y); self._syn()
        time.sleep(hold)
        self.ui.write(e.EV_ABS, e.ABS_HAT0X, 0); self.ui.write(e.EV_ABS, e.ABS_HAT0Y, 0); self._syn()


if __name__ == "__main__":
    p = Pad()
    print("created", p.devnode, "guid", sdl_guid())
    time.sleep(2)
    p.close()
