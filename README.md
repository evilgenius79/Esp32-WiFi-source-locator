# ESP32 WiFi Source Locator

Find where a WiFi signal is coming from. Several Seeed XIAO ESP32 boards act as
passive receivers, each with a directional antenna pointing a different way. An
M5Stack Tab5 compares what they hear and shows you which way to walk.

| Scan: pick a device | Hunt: follow the arrow |
| --- | --- |
| ![Scan screen](docs/scan.png) | ![Hunt screen](docs/hunt.png) |
| **Map: estimate a position** | **Nodes: check and set up the hardware** |
| ![Map screen](docs/map.png) | ![Nodes screen](docs/nodes.png) |

(Screenshots are rendered by the PC preview in `tab5/sim`, using simulated signals.)

> **Status: not yet run on real hardware.** All firmware compiles (XIAO C3, S3,
> C6 and Tab5), the locating math and frame parser pass their unit tests, and the
> screens were checked in the PC preview. Expect some bring-up work on the bench;
> see [First power-up](#first-power-up).

Only use this on networks and devices you own or are authorised to investigate.
The nodes only listen: they never join, probe or disturb the networks they hear.

## How it works

- **Scan.** The nodes hop across channels 1-13 and note every transmitter they
  hear: access points (with their names) and client devices (by MAC address).
  The Tab5 merges the lists; tap one to select it.
- **Hunt.** All nodes lock onto the target's channel and report the signal
  strength of every frame it sends. Each antenna "votes" for its own direction,
  weighted by how strongly it hears the target, which gives the bearing arrow.
  The big number, the bar and the click rate rise as you get closer.
- **Map.** Walk around and tap the map where you are standing at each stop. The
  Tab5 fits a distance-vs-signal model to the readings and paints where the
  source most likely is. Optional battery-powered remote nodes at fixed spots add
  live readings.

It is 2.4 GHz only (none of the C3, S3 or C6 has a 5 GHz radio).

## What you need

| Qty | Part | Notes |
| --- | --- | --- |
| 1 | M5Stack Tab5 | |
| 4 | XIAO ESP32-C3, -S3 or -C6 | Any mix. 3 is the minimum, up to 8 are supported. 4 is the sweet spot: about 8 degrees of bearing error in simulation, against about 14 with 3. |
| 4 | 2.4 GHz directional antennas with U.FL plugs | See [Antennas](#antennas). This is what makes direction finding work. |
| 1 | Grove hub or a hand-made 4-wire harness | All nodes share the same four wires. |
| 0-8 | Extra XIAOs + USB power banks | Optional remote nodes for Map mode. |

### Antennas

Direction finding relies on each node hearing better in one direction than the
others. With the bare stock antennas there is almost no difference between nodes
and the arrow will stay grey ("No clear direction").

- **Easiest:** small 2.4 GHz patch or panel antennas, one per node, facing outwards.
- **Cheapest:** keep the stock stick-on antennas and build a cross-shaped
  divider from cardboard covered in aluminium or copper foil, so each antenna
  sits in its own metal-backed corner, about 3 cm from the foil.
- **XIAO C6:** it starts on its built-in ceramic antenna. Build with
  `-DUSE_EXT_ANTENNA` (below) to switch to the U.FL connector.

Mount the antennas around the Tab5 so they point forward (away from the top
edge when held in landscape), right, back and left. Node 0 is forward and the
ids go clockwise seen from above. Other layouts work too: set each node's
direction on the Nodes tab.

### Wiring

All wired nodes connect in parallel to the Tab5's **Port A** (the red Grove socket):

| Tab5 Port A | XIAO pin | |
| --- | --- | --- |
| GND | GND | |
| 5V | 5V | |
| G53 (SDA) | D4 (SDA) | |
| G54 (SCL) | D5 (SCL) | |

- If no nodes appear, swap the SDA and SCL wires; cable colours are not
  consistent between brands and swapping them is harmless.
- Keep the cables short (under about 50 cm in total). If the *Errors* column on
  the Nodes tab keeps climbing, add a 4.7 kOhm pull-up resistor from SDA to 3V3
  and another from SCL to 3V3 (any XIAO's 3V3 pin will do).
- A XIAO's 5V pin is wired straight to its USB socket. Unplug a node from the
  harness (or at least its 5V wire) before connecting it to a computer.

## Building and flashing

Install [PlatformIO](https://platformio.org/install/cli), then:

```sh
# Each wired node needs its own id, 0 to 7. Pick the environment for the board: c3, s3 or c6.
cd node
PLATFORMIO_BUILD_FLAGS="-DNODE_ID=0" pio run -e c3 -t upload
PLATFORMIO_BUILD_FLAGS="-DNODE_ID=1" pio run -e c3 -t upload
PLATFORMIO_BUILD_FLAGS="-DNODE_ID=2" pio run -e s3 -t upload
PLATFORMIO_BUILD_FLAGS="-DNODE_ID=3 -DUSE_EXT_ANTENNA" pio run -e c6 -t upload

# The Tab5
cd ../tab5
pio run -t upload
```

On Windows PowerShell set the flags with `$env:PLATFORMIO_BUILD_FLAGS="-DNODE_ID=1"` first.

The id is stored on the node, so you can also flash them all identically and
set it afterwards: open `pio device monitor`, type `id 2` and press Enter.
Typing `info` shows the node's id, I2C address and pins.

If the Tab5 will not accept an upload, hold its reset button for about two
seconds until the LED blinks quickly, then try again.

### Optional remote nodes

```sh
cd node
PLATFORMIO_BUILD_FLAGS="-DNODE_ID=0" pio run -e c3_remote -t upload   # shows up as R0
```

A remote node needs only power. It finds the handheld by itself when you start
tracking a target, and appears on the Map and Nodes tabs. Drag it on the map to
where it really is. Remote nodes use ESP-NOW, so in this mode the lowest-numbered
wired node and each remote node send short broadcast packets on the target's channel.

## Using it

1. **Scan** tab: wait a few seconds for the list to fill, filter by access
   points or clients, tap the device you want, then **Hunt** or **Map**.
2. **Hunt:** the first time, tap **Spin cal** and turn once on the spot over about
   12 seconds while holding the Tab5 level. This evens out differences between
   boards and antennas and is remembered. Then follow the arrow, watching that
   the signal number keeps rising. Turn slowly: readings are averaged over about
   half a second.
3. **Map:** choose an **Area** size that covers the space, then walk and tap your
   position at each stop after standing still for two seconds. Three readings
   give a first estimate; eight or more, spread out, give a much better one.
   Lower **Path loss n** in open space, raise it where there are many walls.

Good to know:

- **Quiet targets.** A device only shows up while it transmits. Access points
  always do (about ten times a second). An idle phone may say nothing for many
  seconds; the display shows "no signal" until it speaks again.
- **Private MAC addresses.** Phones that are not connected to a network use
  random, changing addresses and cannot be followed. Once connected, the address
  stays fixed for that network.
- **Wrong channel.** A client's channel is a best guess. If Hunt shows no signal,
  step the channel with **-** / **+**, or hunt the access point it talks to.
- **Reflections.** Indoors, signals bounce. Trust the trend over several steps
  more than any single reading, and expect Map results to be good to a few
  metres, not centimetres.

### First power-up

Because this has not run on hardware yet, check these in order:

1. Tab5 shows the Scan screen in landscape. If it says "No sensor nodes found",
   check the wiring notes above and that Port A is supplying 5 V.
2. Nodes tab lists every node with the right board type and *Errors* near zero.
3. Scan fills with your own access point within a few seconds.
4. Hunt your own access point from a few metres away: every antenna should show
   a reading and *Frames/s* should be around 10 or more.
5. Spin cal, then check the arrow follows as you turn.

Four nodes draw roughly 0.4 A from Port A. If nodes reset or drop out, power
them from a separate 5 V supply with the grounds joined.

## Project layout

| Path | What |
| --- | --- |
| `node/` | XIAO firmware: packet sniffer, I2C slave, ESP-NOW relay / remote node |
| `tab5/` | Tab5 firmware: `main.cpp` (hardware), `app.cpp` (logic and screens) |
| `tab5/sim/` | PC preview that renders the screens to image files |
| `common/` | Shared code: wire protocol, 802.11 frame parser, locating math |
| `test/` | Unit tests that run on a PC |

```sh
# Unit tests
g++ -std=c++17 -Wall -I common test/test_locate.cpp -o /tmp/test_locate && /tmp/test_locate

# Screen preview (after one `pio run` in tab5/); writes .ppm images to tab5/sim/out
tab5/sim/build.sh
```
