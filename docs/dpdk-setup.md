# DPDK Setup Guide

How to prepare a system for DPDK kernel-bypass networking with ephemeral.

## Prerequisites

- Linux kernel 4.14+ (5.x recommended)
- DPDK-compatible NIC: Intel X710/XL710, AWS ENA, Mellanox ConnectX-5+
- Root or CAP_SYS_ADMIN for hugepages and NIC binding
- Compiler with C++23 support (GCC 13+ or Clang 17+)

## 1. Hugepages

DPDK uses hugepages for zero-copy packet buffers. Allocate at boot or at runtime.

### Runtime allocation (2MB pages)

```bash
# Allocate 1024 × 2MB = 2GB hugepages
echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# Verify
grep HugePages /proc/meminfo
# HugePages_Total:    1024
# HugePages_Free:     1024

# Mount hugetlbfs (if not already mounted)
sudo mkdir -p /dev/hugepages
sudo mount -t hugetlbfs nodev /dev/hugepages
```

### Boot-time allocation (persistent)

Add to `/etc/default/grub`:
```
GRUB_CMDLINE_LINUX="default_hugepagesz=2M hugepagesz=2M hugepages=1024"
```
Then `sudo update-grub && reboot`.

### AWS (Graviton / ENA)

ENA on Graviton supports DPDK but requires vfio-pci driver:
```bash
# Amazon Linux 2023
sudo dnf install -y dpdk dpdk-tools numactl-devel

# Allocate hugepages
echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
```

## 2. NIC Binding

DPDK requires the NIC to be unbound from the kernel driver and bound to a DPDK-compatible driver.

### Identify the NIC

```bash
# List network devices and their current drivers
dpdk-devbind.py --status

# Example output:
# 0000:00:05.0 'Elastic Network Adapter (ENA)' if=eth0 drv=ena unused=vfio-pci
```

### Bind to vfio-pci

```bash
# Load vfio-pci module
sudo modprobe vfio-pci

# Unbind from kernel driver and bind to vfio-pci
sudo dpdk-devbind.py -b vfio-pci 0000:00:05.0

# Verify
dpdk-devbind.py --status
# 0000:00:05.0 'Elastic Network Adapter (ENA)' drv=vfio-pci unused=ena
```

**Warning**: Binding the primary NIC to DPDK removes it from the kernel networking stack. Use a secondary NIC, or ensure you have console access (not SSH) before binding.

### For AWS ENA

ENA in DPDK mode requires the `--vdev` EAL argument instead of PCI binding on some instance types:
```bash
# ENA virtual device (if PCI bind doesn't work)
./my_app --vdev=net_ena
```

## 3. EAL Initialization

DPDK's Environment Abstraction Layer (EAL) must be initialized before any DPDK API call.

### Common EAL arguments

```
-l 2,3           # CPU cores to use (comma-separated)
-n 4             # Memory channels
--huge-dir /dev/hugepages
--log-level 5    # Info level (1=Emergency ... 8=Debug)
--no-pci         # Skip PCI scan (use with --vdev)
```

### In ephemeral

```cpp
#include "eph/net/dpdk/eal.hpp"

int main(int argc, char** argv) {
    // eph::net::dpdk::Eal is an RAII wrapper — init on construction, cleanup on destruction.
    eph::net::dpdk::Eal eal{argc, argv};
    // DPDK is now ready — create DpdkPoller, DpdkTcpStream, DpdkUdpSocket, etc.
}
```

### Typical invocation

```bash
./my_app -l 2,3 -n 4 --huge-dir /dev/hugepages -- --host exchange.com
#        ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^    ^^^^^^^^^^^^^^^^^^
#        EAL arguments (before --)                    Application args (after --)
```

## 4. CPU Isolation

For consistent low-latency, isolate CPU cores from the OS scheduler.

### Kernel boot parameter

```
isolcpus=2,3,4,5    # Cores reserved for DPDK lcores
```

### Verify isolation

```bash
# Should show empty mask for isolated cores
taskset -cp $$
# pid 1234's current affinity list: 0,1,6-15   (2-5 isolated)
```

### In ephemeral

`DpdkPoller` pins itself to the lcore passed in its config. The architecture has
no TX worker thread — `send()` runs synchronously on the caller, so you typically
only have one lcore per Poller:

```cpp
auto poller = eph::net::dpdk::DpdkPoller<>::create({
    .port_id  = 0,
    .queue_id = 0,
    .lcore    = 4,   // pin this poller's busy-loop to lcore 4
}).value();
```

Use `eph::utils::cpu::topology()` (see `eph-utils/include/eph/utils/cpu.hpp`) to
discover NUMA topology from code, or run `lscpu --extended` for a quick overview.

## 5. NUMA Locality

On multi-socket servers, allocate memory and pin threads on the same NUMA node as the NIC.

```bash
# Find NIC's NUMA node
cat /sys/bus/pci/devices/0000:00:05.0/numa_node
# 0

# Pin application to NUMA node 0
numactl --cpunodebind=0 --membind=0 ./my_app -l 2,3
```

## 6. Build ephemeral with DPDK

DPDK is sourced from the distribution's system package via pkg-config
(`libdpdk.pc`). Install the OS package and build normally:

```bash
# Arch Linux (includes WSL Arch)
sudo pacman -S dpdk

# Ubuntu / Debian
sudo apt install libdpdk-dev

# From source (production HFT hosts typically pin a specific version)
# Disable the openssl crypto PMD to avoid dragging openssl into the link,
# plus any PMD you don't need:
meson setup build -Ddisable_drivers=crypto/openssl
ninja -C build && sudo ninja -C build install
```

Then:

```bash
xmake f -m release -y

# Build DPDK examples (see examples/ and eph-net-dpdk/tests/)
xmake build simple_hft_dpdk
xmake build ws_echo_client_dpdk
```

Historical note: the previous vcpkg::dpdk path flattened openssl headers into
the same include tree as DPDK, which conflicted with aws-lc. A compiler
wrapper at `/tmp/gcc14-wrap/g++` was used to reorder `-isystem` / `-L` flags
so aws-lc resolved first. With system libdpdk the conflict is gone (isolated
`/usr/include/dpdk/` layout) and the wrapper is **no longer required**.

## 7. Verification

### Quick smoke test

```bash
# Build a DPDK example
xmake build simple_hft_dpdk

# Run with EAL args (adjust for your setup)
sudo xmake run simple_hft_dpdk \
    -l 2,3 --huge-dir /dev/hugepages -- \
    --host echo.websocket.org --local-ip 10.0.0.2 --gateway 10.0.0.1

# Or use the latency benchmark wrapper (handles NIC-B state transitions)
sudo ./benchmarks/latency/lat tcp --dpdk
```

### Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `EAL: No free hugepages` | Hugepages not allocated | Allocate: `echo 1024 > .../nr_hugepages` |
| `EAL: Cannot init memory` | No hugetlbfs mount | Mount: `mount -t hugetlbfs nodev /dev/hugepages` |
| `EAL: Detected X NUMA nodes` mismatch | Wrong NUMA binding | Use `numactl --cpunodebind=N` |
| `PMD: net_ena: Failed to init` | ENA not bound to vfio-pci | Bind: `dpdk-devbind.py -b vfio-pci <pci>` |
| `ARP resolution failed` | Gateway unreachable | Check IP config, routing, security groups |
| `TCP handshake timeout` | Firewall blocking | Check server-side firewall allows the source IP |
