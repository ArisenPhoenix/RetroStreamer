# ArchStreamer client VM (second computer on libvirt NAT)

Use a desktop Ubuntu VM as a real remote client with its own GStreamer video/audio windows.
This is better than [`deploy/lan-sim/`](../lan-sim/README.md) when you need to **see** media lag, while still avoiding a second physical PC.

## Layout

| Role | Where | Address |
|------|--------|---------|
| Host | metal (`host_runner` / GUI Host tab) | listens on all interfaces; NAT gateway **`192.168.122.1`** |
| Local watch | metal “Watch stream locally” | `127.0.0.1` (loopback — not the same path as the VM) |
| Client | VM `archstreamer-client` | `192.168.122.x` on libvirt network `default` |

```text
Metal host ── virbr0 192.168.122.1 ── NAT ── VM client 192.168.122.x
     │
     └── Watch stream locally → 127.0.0.1 (separate, near-zero network lag)
```

Artwork stays local on each side (not transferred over the control channel).

## Prerequisites (metal)

Already applied on this machine if you followed the setup plan:

- QEMU/KVM, libvirt, virt-manager, virt-viewer
- User in groups `libvirt` and `kvm` (log out/in if `virsh -c qemu:///system` fails)
- Default network `default` active (`virbr0`, gateway `192.168.122.1`)
- ISO + disk under  
  `/mnt/Internal_SSD/Programming/Mixed/ArchStreamer-VMs/archstreamer-client/`

Repair / recreate with:

```bash
./deploy/vm-client/host-setup.sh
```

Open the VM window:

```bash
./deploy/vm-client/open-console.sh
# or: virt-manager
```

## Guest Ubuntu install (one-time)

1. Complete the Ubuntu 24.04 Desktop installer in the virt-viewer window.
2. After first boot, open a terminal **in the guest**.
3. Copy or clone this repo, then:

```bash
# If the metal repo is not shared into the guest, clone from GitHub:
curl -fsSL https://raw.githubusercontent.com/ArisenPhoenix/RetroStreamer/main/deploy/vm-client/guest-bootstrap.sh -o guest-bootstrap.sh
# Or, after cloning the repo in the guest:
bash deploy/vm-client/guest-bootstrap.sh
```

`guest-bootstrap.sh` installs deps, configures **`-DARCHSTREAMER_BUILD_HOST=OFF`**, and builds `session_client` / `archstreamer_gui`.

Default clone URL: `https://github.com/ArisenPhoenix/RetroStreamer.git`  
Override with `ARCHSTREAMER_REPO_URL=...` if needed.

## Firewall (metal)

Guest traffic hits the host via `virbr0`. If UFW/firewalld blocks LAN ports, open them:

```bash
sudo ./scripts/apply-firewall.sh
```

Ports: TCP `45555`, UDP `45454` (input), `45550` (discovery), `5004–5011` video, `6004–6011` audio.

Inside the guest, media UDP is inbound to the guest; Ubuntu Desktop usually allows this for a local user session. If the video window never appears, open the same UDP ranges in the guest firewall as well.

## Smoke test: media lag (host local vs VM)

Goal: confirm the VM client is on a **non-loopback** path, and compare absolute lag to metal “Watch stream locally”.

### Metal

1. Start ArchStreamer GUI (or `host_runner`).
2. Host tab: pick a game, **Host role = Viewer**, stream video + audio on.
3. Enable **Watch stream locally** (this is the low-latency reference).
4. Optionally enable **Synced A/V** on the Client tab — that setting also drives local watch when toggled.
5. Start Host and wait until the session is live / accepting clients.

### Guest VM

1. Run `archstreamer_gui` or `session_client`.
2. Set host to **`192.168.122.1`** (not `127.0.0.1`).
3. Match mode/game; Connect → Join Session.
4. Use the **same Synced A/V** setting as on the metal Client tab if you care about lip-sync.

CLI example from the guest after bootstrap:

```bash
~/ArchStreamer/build/session_client \
  --host 192.168.122.1 --port 45555 --input-port 45454 \
  --username vm_client --role player --mode singleplayer --players 1 \
  --game 0 --synced-av
```

### What to look for

| Observation | Meaning |
|-------------|---------|
| Metal local watch A/V in sync with itself; VM A/V in sync with itself; VM **behind** local watch | Expected — NAT/virtio path vs loopback |
| Client audio behind client video (Synced A/V off) | Legacy dual pipeline; enable Synced A/V to lip-sync |
| Synced A/V on: both streams delayed together on that machine | Shared-clock path working |
| No video in VM | Firewall / wrong host IP / host not streaming video |

Phone-cam both windows while opening a sharp in-game menu is enough to judge tens-of-ms gaps.

## Useful virsh commands (metal)

```bash
sg libvirt -c 'virsh -c qemu:///system list --all'
sg libvirt -c 'virsh -c qemu:///system start archstreamer-client'
sg libvirt -c 'virsh -c qemu:///system shutdown archstreamer-client'
sg libvirt -c 'virsh -c qemu:///system net-list --all'
```

## Why not only Docker lan-sim?

[`deploy/lan-sim`](../lan-sim/README.md) is great for TCP/UDP smoke tests with a second IP stack, but it is headless and bind-mounts host binaries. This VM is for **visual** cross-play and media-lag judgment.
