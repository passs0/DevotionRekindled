# DevotionRekindled — Technical Reference & Maintainer Notes

This document preserves all reverse-engineered hardware constants, HID report offsets, InControl profile mappings, and Mono hook details required to maintain or update **DevotionRekindled** (`libds4_shim.dylib` & `DevotionRekindled.app`).

---

## 1. Project Layout

- [`ds4_shim.c`](ds4_shim.c): Universal (`x86_64` + `arm64`) dynamic library shim injected via `DYLD_INSERT_LIBRARIES`.
- [`launcher.c`](launcher.c): Native launcher executable inside `DevotionRekindled.app/Contents/MacOS/DevotionRekindled` that sets `DYLD_INSERT_LIBRARIES` to the bundled `libds4_shim.dylib` and `execv`s `/Applications/Devotion.app/Contents/MacOS/Devotion`.
- [`Makefile`](Makefile): Builds universal binaries (`libds4_shim.dylib` and `DevotionRekindled.app`) and signs them ad-hoc.
- [`scripts/make_icon.m`](scripts/make_icon.m): Objective-C icon builder that extracts the exact macOS stock squircle mask, Liquid Glass specular rim profile, and drop shadow from `/System/Applications/Games.app`.
- [`Restore Devotion Saves.command`](Restore%20Devotion%20Saves.command): One-click utility to restore `~/Library/Containers/inc.redcandlegames.devotion` saves and `PlayerPrefs`.

---

## 2. DualSense Bluetooth HID Report `0x31` Layout (`UsagePage 0xFF00`, `Usage 0x3B`)

On macOS Bluetooth, the Sony DualSense Wireless Controller (`VID 0x054C`, `PID 0x0CE6`) encapsulates its full input state inside a **77-byte vendor-defined HID element** (`UsagePage 0xFF00`, `Usage 0x3B`), while standard HID elements (`0x01:0x32` Z, `0x01:0x35` Rz, buttons `0x09:1..14`, hat switch `0x01:0x39`) report `0`.

| Offset in `ds_data` (77 bytes) | Field | Range / Bitmask | Notes |
| :--- | :--- | :--- | :--- |
| `ds_data[0]` | BT Header / Tag | `0x31` (or `0x01`) | Validated by checking `len == 77` |
| `ds_data[1]` | Left Stick X (`LX`) | `0x00..0xFF` (center `0x80`) | `0x00` = Left, `0xFF` = Right |
| `ds_data[2]` | Left Stick Y (`LY`) | `0x00..0xFF` (center `0x80`) | `0x00` = Up, `0xFF` = Down |
| `ds_data[3]` | Right Stick X (`RX`) | `0x00..0xFF` (center `0x80`) | `0x00` = Left, `0xFF` = Right |
| `ds_data[4]` | Right Stick Y (`RY`) | `0x00..0xFF` (center `0x80`) | `0x00` = Up, `0xFF` = Down |
| `ds_data[5]` | L2 Analog Trigger | `0x00..0xFF` | `0x00` = Released, `0xFF` = Full press |
| `ds_data[6]` | R2 Analog Trigger | `0x00..0xFF` | `0x00` = Released, `0xFF` = Full press |
| `ds_data[8] & 0x0F` | D-Pad Hat Switch | `0..7` (`8` = Neutral) | `0`=N, `1`=NE, `2`=E, `3`=SE, `4`=S, `5`=SW, `6`=W, `7`=NW |
| `ds_data[8] & 0xF0` | Face Buttons | Bitmask | `0x10`=Square(1), `0x20`=Cross(2), `0x40`=Circle(3), `0x80`=Triangle(4) |
| `ds_data[9]` | Shoulder / Options / L3 / R3 | Bitmask | `0x01`=L1(5), `0x02`=R1(6), `0x04`=L2_btn(7), `0x08`=R2_btn(8), `0x10`=Share(9), `0x20`=Options(10), `0x40`=L3(11), `0x80`=R3(12) |
| `ds_data[10]` | System / Touchpad | Bitmask | `0x01`=PS(13), `0x02`=Touchpad(14) |
| `ds_data[11..14]` | 32-bit Packet Counter | `uint32_t` LE | Monotonic hardware counter; used in `parse_dualsense_payload` to skip duplicate reads in O(1) |

---

## 3. Unity + InControl Profile & Axis Mapping

*Devotion* uses **Unity 2020.2.1f1** (`x86_64` running under Rosetta 2) with **InControl**.
- When `GCController` is swizzled to report `vendorName = "Sony Interactive Entertainment"` and `productCategory = "Wireless Controller"`, Unity's `Input.GetJoystickNames()` returns `"Sony Interactive Entertainment Wireless Controller"`.
- This matches InControl's **`PlayStation4MacBTUnityProfile`**, which expects:
  - **Buttons**: `joystick button 0..13` (Square=0, Cross=1, Circle=2, Triangle=3, L1=4, R1=5, L2_btn=6, R2_btn=7, Share=8, Options=9, L3=10, R3=11, PS=12, Touchpad=13).
  - **D-Pad**: Mapped via `Analog4` / `Analog5` or hat switch / synthetic key events in Mono hooks.
  - **Analog Axes (`analog 0..5`)**:
    - `analog 0` (`X` `0x01:0x30`): Left Stick X (`[-1.0, +1.0]`)
    - `analog 1` (`Y` `0x01:0x31`): Left Stick Y (`[-1.0, +1.0]`)
    - `analog 2` (`Z` `0x01:0x32`): Right Stick X (`[-1.0, +1.0]`)
    - `analog 3` (`Rz` `0x01:0x35`): Right Stick Y (`[-1.0, +1.0]`)
    - `analog 4` (`Rx` `0x01:0x33`): **L2 Trigger** (mapped to `[-1.0, +1.0]` where `-1.0` = released, `+1.0` = full press; InControl applies `(v + 1.0) * 0.5`)
    - `analog 5` (`Ry` `0x01:0x34`): **R2 Trigger** (mapped to `[-1.0, +1.0]` where `-1.0` = released, `+1.0` = full press)
- Meanwhile, *Devotion*'s Main Menu UI polls Unity's legacy `Input.GetAxis("LeftStickX")`, `Input.GetAxis("RightStickX")`, `Input.GetAxis("LeftTrigger")`, `Input.GetAxis("RightTrigger")`, and `Input.GetKeyInt` directly, which are intercepted via `mono_add_internal_call` hooks.

---

## 4. Critical Pitfalls Avoided

1. **Never `CFRelease` the Discovery `IOHIDManagerRef` (`g_disc_mgr`)**:
   - Calling `CFRelease(g_disc_mgr)` after opening the DualSense device closes all `IOHIDDeviceRef` instances owned by that manager and silently halts `g_priv_queue`.
2. **Track All `IOHIDDeviceRef` Instances (`g_known_ds_devs[16]`)**:
   - Unity creates its own `IOHIDManagerRef` and thus receives a distinct `IOHIDDeviceRef` pointer for the same physical controller. Tracking all matched DualSense `IOHIDDeviceRef` pointers ensures `IOHIDDeviceGetValue` and `IOHIDQueueCopyNextValueWithTimeout` intercept Unity's device instance.
3. **Coalesced Queue Synthesis (`g_axis_dirty[0..5]`) & Immediate Raw Queue Draining**:
   - DualSense transmits Bluetooth reports at 250Hz–500Hz. Enqueuing 6 synthetic `IOHIDValueRef`s per report into a FIFO buffer caused a multi-second Right Stick latency backlog because Unity only pops a bounded number of queue events per frame.
   - Instead, `my_IOHIDQueueCopyNextValueWithTimeout` drains all pending raw queue events immediately on every call and synthesizes at most **one** `IOHIDValueRef` per dirty axis (`g_axis_dirty[i]`), delivering the latest hardware stick position with zero queue lag.
4. **Never Fallback to `orig_GetAxis` for Right Stick when Centered**:
   - Calling `orig_GetAxis` when the Right Stick is inside the deadzone returns Unity's stale smoothed `InputManager` axis value (`gravity=3`), causing camera drift after releasing the stick. Always return `0.0f` directly for `RightStickX`/`RightStickY` when inside the deadzone.
