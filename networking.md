# Networking (TAP bridging for the Indy guest)

The launchers (`run-irix.sh`, `run-irix.bat`) attach the guest's ec0
NIC to a host TAP interface in bridged mode, which makes the guest a
full peer on your LAN. If the TAP interface is not there, the scripts
say so and boot the guest **without networking** — everything else
works. This document explains how to set TAP up when you want it.

The guest side is already configured: Linux uses interface `tap-iris`,
Windows uses an adapter named `my-tap`. Names matter — use exactly
these unless you override them (`IRIX_TAP_IF` on Linux).

## Windows

1. **Install the TAP driver.** It comes with OpenVPN: download the
   installer from <https://openvpn.net/community-downloads/> (you only
   need the TAP-Windows6 driver, not a VPN account). Installing a driver
   needs administrator rights.
2. **Rename the adapter to `my-tap`.** Control Panel → Network and
   Internet → Network Connections, right-click the new TAP adapter →
   Rename.
3. **Bridge it to your LAN.** Select the `my-tap` adapter *and* your
   Ethernet adapter (Ctrl-click), right-click → Bridge Connections.
   Windows creates a Network Bridge; give the machine a minute to come
   back on the network. (Bridging a *Wi-Fi* adapter rarely works on
   Windows — use wired Ethernet for the bridge.)
4. Re-run `install.bat` — it should now report `TAP adapter 'my-tap'
   found` — then start the guest with `run-irix.bat`.

## Linux

Everything below needs root (or sudo). Interface names are examples;
the guest side expects `tap-iris` by default.

1. **Create the TAP interface owned by your user:**
   ```sh
   sudo ip tuntap add dev tap-iris mode tap user $USER
   ```
2. **Create a bridge and enslave your Ethernet NIC plus the TAP:**
   ```sh
   sudo ip link add br0 type bridge
   sudo ip link set eth0 master br0       # replace eth0 with your NIC
   sudo ip link set tap-iris master br0
   sudo ip link set eth0 up
   sudo ip link set tap-iris up
   sudo ip link set br0 up
   ```
3. **Move your LAN address onto the bridge.** The NIC enslaved to a
   bridge stops working with its own address — renew DHCP (or assign
   your static address) on `br0` instead:
   ```sh
   sudo dhclient br0
   ```
   To survive reboots, configure `br0` in your distro's network manager
   (netplan, NetworkManager, systemd-networkd) — see its documentation.
4. Start the guest with `./run-irix.sh` and give IRIX an address on
   your LAN.

## Alternatives (no TAP needed)

- `IRIX_NETMODE=user ./run-irix.sh` (Linux) — SLIRP user-mode
  networking, if the build includes it. No host setup at all, but the
  guest is behind NAT (inbound connections don't reach it).
- `IRIX_NONET=1` (Linux) — drop the NIC entirely. Useful to isolate
  network-dependent hangs.
