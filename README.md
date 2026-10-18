# scopehal-sigrok-bridge (libsigrok4DSL / ALL-LOGIC)

Exposes a device supported by **libsigrok4DSL** — the device layer of
[DSView](https://github.com/DreamSourceLab/DSView) and
[ALL-LOGIC](https://github.com/dovandung/ALL-LOGIC) — to
[ngscopeclient](https://github.com/ngscopeclient/scopehal-apps) as a network
instrument. ngscopeclient talks to it with its `dslabs` driver over the
`twinlan` transport: SCPI on port 5025, waveforms on port 5026.

This fork targets the current libsigrok4DSL public API (`ds_lib_init`,
`ds_active_device`, `ds_start_collect`, ...) used by DSView 1.3+ and ALL-LOGIC.
The original 2022 bridge used the older `sr_session_*` API, which those
versions no longer export.

## Supported devices

Every logic analyzer driver in the libsigrok4DSL tree it is built against.
With ALL-LOGIC that is:

| Driver | Hardware |
|---|---|
| `DSLogic` | DreamSourceLab DSLogic |
| `ch32` | CH32H417 logic analyzer |
| `slogic16u3` | Sipeed SLogic Combo 8 / 16U3 / 32U3 |
| `pxlogic` | PXLogic32U3 |
| `atk-logic` | Alientek ATK-Logic (DL16 ...) |
| `fx2lafw` | nanoDLA / Cypress FX2 |
| `cypress-fx3` | Cypress FX3 |
| `hsdaoh-la` | RP2350 + MS2130 (only if `libhsdaoh` is found) |
| `virtual-demo` | built-in demo device, no hardware needed |

DSCope (DSO mode) is supported through the analog path: each `SR_DF_DSO` frame
is published as it arrives. It is tested with the demo device in DSO mode
(`--mode dso`), not on DSCope hardware.

## Build (Linux)

Dependencies: CMake, a C++17 compiler, glib-2.0, libusb-1.0, zlib, hidapi
(`libhidapi-dev`), and an ALL-LOGIC (or DSView) source checkout. libsigrok4DSL
is compiled from that checkout; nothing needs to be installed from it.

```sh
sudo apt install build-essential cmake pkg-config libglib2.0-dev libusb-1.0-0-dev zlib1g-dev libhidapi-dev
git clone https://github.com/dovandung/ALL-LOGIC
git clone --recurse-submodules https://github.com/dovandung/scopehal-sigrok-bridge
cd scopehal-sigrok-bridge
cmake -B build -DALLLOGIC_DIR=../ALL-LOGIC
cmake --build build -j
```

Devices need the same udev rules / firmware files as ALL-LOGIC. Point `--res`
at the directory holding the firmware/bitstreams (DSView's `res` folder).

## Run

```sh
./build/scopehal-sigrok-bridge --list          # show detected devices
./build/scopehal-sigrok-bridge slogic          # by name substring
./build/scopehal-sigrok-bridge fx2lafw         # or by driver name
./build/scopehal-sigrok-bridge 0               # or by index
```

Options: `--port N` (SCPI port, data is N+1), `--res DIR`, `--mode logic|dso`
(switch the device mode first; DSCope and the demo device have both), `--max-depth N`
(largest memory depth offered, default 100M or the driver's hardware limit),
`--sr-log 0..5`, `-q`.

In ngscopeclient: **Add → Oscilloscope**, driver `dslabs`, transport `twinlan`,
path `localhost:5025` (or the bridge host's address).

### ngscopeclient patch

ngscopeclient's `dslabs` driver only knows two model names (DSCope U3P100 and
DSLogic U3Pro16) and creates no channels for anything else.
[`client-patches/scopehal-dslabs-generic-bridge.patch`](client-patches/scopehal-dslabs-generic-bridge.patch)
makes it ask the bridge (`ANACHANS?` / `DIGCHANS?`) instead, and fixes three
decode bugs in the same driver:

- the analog path stored a null buffer pointer, then converted from it;
- the digital path skipped bits 1-7 of the first byte;
- the digital path dropped the last stored sample (`Resize(k)` with `k` an index).

Apply it to scopehal-apps:

```sh
cd scopehal-apps/lib && git apply /path/to/scopehal-dslabs-generic-bridge.patch
```

## Testing without ngscopeclient

`tools/test_client.py` speaks the same protocol as the `dslabs` driver:

```sh
./build/scopehal-sigrok-bridge demo &
python3 tools/test_client.py --count 10 --depth 100000
```

## How it works

- **Capture loop.** `ds_start_collect()` runs one capture on a library thread.
  Drivers deliver it as `SR_DF_LOGIC` chunks (`LA_CROSS_DATA`: 64-sample groups,
  8 bytes per enabled channel), an optional `SR_DF_TRIGGER`, then `SR_DF_END`.
  The bridge accumulates the chunks per channel and publishes the waveform only
  when the capture is complete, then starts the next capture.
- **Trigger position.** Drivers built on `capture_core` (FX3, SLogic, hsdaoh)
  report the trigger's sample index; PXLogic sends the trigger packet with no
  payload at the trigger point; drivers that send none put the trigger at the
  configured pre-trigger percentage. The digital data is shifted so the trigger
  sample lands at the client's trigger offset.
- **Reconfiguration.** Rate, depth and channel changes abort the running
  capture. Any waveform taken with the old settings is discarded; the next
  capture uses the new ones.
- **Thread housekeeping.** libsigrok4DSL never joins its collect thread when a
  capture ends on its own. The bridge joins it, so long sessions don't leak
  thread stacks.

## Wire protocol (data port)

The client sends one byte `K` per waveform it wants. The bridge answers, in
host byte order:

```
uint32 seqnum | uint16 numchans | int64 fs_per_sample | int64 trigger_fs | double hw_wfms_per_s
per channel:
  size_t chnum | size_t memdepth
  digital: int32 first_sample | uint8 data[memdepth]        (8 samples per byte, LSB first)
  analog:  float scale, offset, trigphase | bool clipping | uint8 adc[memdepth]
```

`size_t`/`bool` are sent raw, so bridge and client must share an architecture.

## SCPI additions

On top of the common bridge commands (`START`, `STOP`, `RATE`, `DEPTH`,
`TRIG:*`, `<ch>:ON|OFF|THRESH`, `RATES?`, `DEPTHS?`, `ARMED?`, ...):

- `ANACHANS?` — number of analog channels
- `DIGCHANS?` — number of digital channels
- `CHANS?` — total channel count (unchanged)

Channels are addressed by index (`0` ... `31`). `THRESH` maps to the device's
single threshold (`SR_CONF_VTH`) on drivers that have one.
