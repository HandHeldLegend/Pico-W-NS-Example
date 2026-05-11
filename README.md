# Pico-W-NS-Example

`v0.1.0`

This repository is a Raspberry Pi Pico W example that uses
[`NS-LIB-HID`](https://github.com/HandHeldLegend/NS-LIB-HID) to present
Switch-style controller behavior over either USB or Bluetooth Classic HID.

The example is intentionally small: it focuses on the protocol glue, the
transport loops, and pairing-data persistence rather than on a full input
stack. In other words, this project is meant to show how to wire the library
into real firmware, not to be a finished controller product.

## What This Example Demonstrates

- Initializing `NS-LIB-HID` with a concrete device configuration.
- Selecting USB or Bluetooth mode at boot using GPIO straps.
- Forwarding host output reports into the library with `ns_api_output_tunnel()`.
- Generating 64-byte input reports at the expected cadence with
  `ns_api_generate_inputreport()`.
- Saving pairing credentials to flash so Bluetooth reconnect can work across
  resets.
- Providing the application callbacks that `NS-LIB-HID` expects firmware to own.

## Current Example Scope

This example is intentionally minimal. Out of the box it currently does the
following:

- Exposes two example buttons:
  - `A` on `GP14`
  - `B` on `GP15`
- Keeps both analog sticks centered.
- Leaves IMU reporting unimplemented.
- Leaves player LEDs, shutdown handling, and haptics as stubs.
- Stores host MAC + link key when pairing data becomes available.
- Uses a fixed example device MAC address in `main.c`.
- Configures the library with `NS_DEVTYPE_SNES_JP` in `main.c`.

That makes the project a good transport and protocol example, but not yet a
full-featured controller implementation.

## Hardware Assumptions

The example is written for a Raspberry Pi Pico W and assumes simple active-low
inputs with internal pull-ups enabled in firmware.

Boot-mode pins:

- `GP0`: hold low during boot to enter Bluetooth reconnect mode.
- `GP1`: hold low during boot to enter Bluetooth pairing mode.
- If neither pin is held low at boot, the example enters USB mode.

Example input pins:

- `GP14`: `A` button, active low.
- `GP15`: `B` button, active low.

Debug UART:

- `GP12`: UART TX
- `GP13`: UART RX

Those UART pins were chosen so `GP0` and `GP1` stay available as boot straps.

## Boot Modes

The entire example revolves around a simple boot-time transport choice:

1. `USB mode`
   Default mode when both boot pins are left high. The firmware starts TinyUSB,
   enumerates using descriptors supplied by `NS-LIB-HID`, and sends reports on
   the USB HID path.

2. `Bluetooth reconnect mode`
   Entered by holding `GP0` low during boot. The firmware starts the Pico W
   wireless stack, configures BTstack as a HID device, restores saved pairing
   credentials, and attempts to reconnect using the stored host information.

3. `Bluetooth pairing mode`
   Entered by holding `GP1` low during boot. This is the discovery/bonding path
   intended for first-time wireless pairing or re-pairing.

This split keeps the runtime simple: transport is chosen once at startup, and
the rest of the firmware can run a single transport loop without mode switching
in the background.

## Typical Bring-Up Flow

For a first pass, the easiest way to use the example is:

1. Build and flash the firmware onto a Pico W.
2. Wire momentary switches so `GP14` and `GP15` can be pulled to ground.
3. Boot normally to enter USB mode.
4. Connect the board to the host or Switch over USB and confirm the device
   enumerates.
5. If you want wireless operation, pair once so the host MAC and link key can
   be captured and written to flash.
6. Reboot while holding `GP0` low to test Bluetooth reconnect behavior.
7. Reboot while holding `GP1` low whenever you want to force wireless pairing
   mode instead.

## Pairing and Persistent Storage

One of the most important things this example shows is that transport handling
alone is not enough for a usable wireless controller. The firmware also has to
remember pairing material.

The stored structure is defined in `main.h`:

- a magic value used to detect initialized storage
- the last known host MAC address
- the 16-byte link key

The example stores that data in flash through `ns_flash.c`. Writes are deferred
through `ns_flash_task()` instead of writing immediately from a callback. That
design matters because flash erase/program operations are sensitive on RP2040
systems and are safer when funneled through one controlled path.

`ns_set_usbpair_cb()` is the application hook that copies the pairing data into
`device_storage` and schedules the write. In this example that callback is used
as the central place to preserve credentials, regardless of whether they were
learned from USB-assisted pairing or the Bluetooth stack.

## How The Firmware Is Structured

### `main.c`

`main.c` is the top-level orchestrator.

It:

- reads the boot pins to choose transport
- initializes stdio and flash support
- loads previously saved pairing data
- fills `ns_device_config_s`
- calls `ns_lib_init()`
- hands execution to either `ns_usb_enter()` or `ns_btc_enter()`

It also implements the callback surface that `NS-LIB-HID` expects platform
firmware to own, such as:

- `ns_get_inputdata_cb()`
- `ns_get_powerstatus_cb()`
- `ns_set_usbpair_cb()`
- `ns_set_led_cb()`
- `ns_set_imumode_cb()`
- `ns_set_haptic_indices_cb()`
- `ns_get_time_ms()`
- `ns_get_random_u8()`

That division of responsibility is deliberate: `NS-LIB-HID` handles the Switch
protocol and descriptor content, while the application remains responsible for
real hardware state, timing, persistence, and side effects.

### `ns_usb.c`

`ns_usb.c` is the wired transport path.

Its job is simple:

- initialize TinyUSB
- feed received host output reports into `ns_api_output_tunnel()`
- call `ns_api_generate_inputreport()` at the right cadence
- submit the resulting HID report back over USB

The cadence is synchronized to USB start-of-frame events so the example lands
close to the Switch's expected 8 ms report interval. That is why the file uses
`tud_sof_cb()` and a `_frame_ready` flag instead of blindly transmitting as fast
as the main loop can run.

### `ns_btc.c`

`ns_btc.c` is the wireless transport path.

It:

- brings up the CYW43 + BTstack stack
- configures GAP/HID/SDP state
- publishes the HID descriptor supplied by `NS-LIB-HID`
- forwards host HID output data into `ns_api_output_tunnel()`
- requests "can send now" events and emits input reports at roughly 8 ms
- stores or restores link-key material as needed

The Bluetooth loop mirrors the same core library contract as USB: host output
goes in through `ns_api_output_tunnel()`, and outgoing reports come from
`ns_api_generate_inputreport()`. That symmetry is a major reason this example is
useful; once the application callbacks are implemented, the transport-specific
code stays relatively thin.

### `ns_flash.c`

`ns_flash.c` is a tiny persistence helper.

It provides:

- `ns_flash_read()` to load a saved structure from flash
- `ns_flash_write()` to queue a write request
- `ns_flash_task()` to perform the queued write safely

Keeping flash behavior isolated makes the rest of the example easier to reason
about and gives you one obvious place to replace if your product later moves to
EEPROM, FRAM, a file system, or a different flash layout.

## Why It Is Built This Way

The example has a few design choices that are worth calling out explicitly:

- `Boot-time mode selection instead of runtime switching`
  This keeps the demo predictable and easy to debug. USB and Bluetooth each get
  a dedicated loop, and there is no need to hot-switch stacks after startup.

- `Library-owned protocol, application-owned hardware hooks`
  `NS-LIB-HID` is responsible for protocol details, descriptor data, and report
  formatting. The application is responsible for the parts only it can know:
  buttons, storage, LEDs, haptics, IMU data, and timing.

- `Persist pairing data in flash`
  Wireless reconnect only becomes practical when the controller can remember the
  console identity and key material across resets.

- `Maintain an 8 ms report cadence`
  Both transport files are written around the idea that stable report timing is
  part of making host communication behave like a real controller.

## Build Requirements

You will need:

- Raspberry Pi Pico SDK `2.2.0` or a compatible setup
- CMake `3.13+`
- An ARM GCC toolchain suitable for Pico development
- Git submodule support for `external/NS-LIB-HID`

Because `NS-LIB-HID` is included as a git submodule, make sure it is present
before building:

```bash
git submodule update --init --recursive
```

## Building From The Command Line

From the repository root:

```bash
mkdir build
cd build
cmake .. -DPICO_SDK_PATH=/path/to/pico-sdk
cmake --build .
```

Expected outputs are generated into the build directory by
`pico_add_extra_outputs(...)`, including the UF2 file used for drag-and-drop
flashing.

## Building In VS Code

This project was generated from the Pico VS Code extension template and already
includes the standard `pico_sdk_import.cmake` plumbing.

If your Pico SDK environment is already set up in the IDE, you can generally:

1. Open the repository folder.
2. Configure CMake.
3. Build the `Pico-W-NS-Example` target.
4. Flash the produced UF2 to a Pico W in BOOTSEL mode.

## Flashing

The usual RP2040 flow applies:

1. Hold `BOOTSEL` while connecting the Pico W to USB.
2. Copy the generated `.uf2` file onto the mounted mass-storage device.
3. The board will reboot into the example firmware.

## Operating The Example

### USB mode

- Leave `GP0` and `GP1` unasserted.
- Power or reset the board.
- The firmware enters `ns_usb_enter()`.
- Host output reports are tunneled into `NS-LIB-HID`.
- Input reports are generated on the USB HID path.

### Bluetooth reconnect mode

- Hold `GP0` low while powering or resetting the board.
- The firmware enters `ns_btc_enter(..., false)`.
- Saved pairing data is used to restore link-key context and reconnect.

### Bluetooth pairing mode

- Hold `GP1` low while powering or resetting the board.
- The firmware enters `ns_btc_enter(..., true)`.
- The Bluetooth HID device becomes pairable/discoverable for a fresh bond.

## Debugging

The example enables Pico SDK stdio on UART and disables USB stdio:

- UART enabled
- USB stdio disabled

That means boot logs and pairing/debug prints are expected on the configured
UART pins (`GP12`/`GP13`), not over the USB CDC console.

Useful things the firmware prints today include:

- the stored host MAC address
- the stored link key
- whether the board entered USB or Bluetooth mode
- Bluetooth pairing and connection events

## Important Things To Customize In A Real Product

If you use this example as a base, these are the first things you will normally
replace:

- `device_mac` in `main.c` with a unique per-device address policy
- the placeholder button/stick mapping in `ns_get_inputdata_cb()`
- `ns_get_powerstatus_cb()` with real battery and charge reporting
- `ns_set_led_cb()` with actual LED behavior
- `ns_set_haptic_indices_cb()` with motor/actuator handling
- `ns_get_imu_standard_cb()` / `ns_get_imu_quaternion_cb()` with sensor data
- the selected `NS_DEVTYPE_*` identity in `main.c`

## Licensing

Unless otherwise noted, the example application code and original documentation
in this repository are Copyright (c) 2026 Hand Held Legend, LLC, authored by
Mitchell Cairns, and licensed under
[CC BY-NC 4.0](https://creativecommons.org/licenses/by-nc/4.0/).

That means the example content may be shared and adapted for non-commercial use
with proper attribution and indication of changes.

For commercial licensing inquiries, contact `support@handheldlegend.com`.

Important scope note:

- The bundled `external/NS-LIB-HID` library is not covered by the top-level
  example license; it carries its own licensing terms and notices.
- Some template-derived or third-party files may retain their original notices
  where required.

See the repository `LICENSE` file for the project-level license notice and
scope.

## Repository Layout

- `main.c` / `main.h`: boot logic, configuration, and application callbacks
- `ns_usb.c`: TinyUSB transport path
- `ns_btc.c`: Pico W Bluetooth HID transport path
- `ns_flash.c`: flash-backed pairing-data persistence
- `external/NS-LIB-HID`: bundled protocol/descriptor library
- `btstack_config.h`: BTstack feature sizing and configuration
- `tusb_config.h`: TinyUSB device configuration

## Notes For Reviewers

For release `v0.1.0`, the value of this repository is mainly educational:

- it shows the smallest complete path from `NS-LIB-HID` init to live transport
- it demonstrates where pairing data is captured and why it must be persisted
- it makes the transport/library boundary clear enough to extend into real
  hardware

If you are trying to understand the code quickly, start with `main.c`, then
read `ns_usb.c` and `ns_btc.c` as two parallel transport adapters wrapped
around the same library API.
