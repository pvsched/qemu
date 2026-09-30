# pvsched QEMU fork

This fork adds the **pvsched paravirtualized-scheduling PCI device**
(`hw/i386/pvsched.c`), the discovery transport between a pvsched guest kernel
and the host kernel's pvsched framework. The device commit describes its
interface. Companion repositories:
[pvsched/linux](https://github.com/pvsched/linux) and
[pvsched/bpf-policy](https://github.com/pvsched/bpf-policy).

## Build

```sh
sudo apt-get install ninja-build libglib2.0-dev libpixman-1-dev libslirp-dev
./configure --target-list=x86_64-softmmu --enable-kvm --enable-slirp
ninja -C build qemu-system-x86_64
```

`libslirp` is only needed for `-nic user,...` guest networking.

## Run

```sh
build/qemu-system-x86_64 -enable-kvm ... -device pvsched
```

The host needs the Linux 7.2 pvsched framework loaded (`/dev/pvsched`); the
guest needs `CONFIG_PARAVIRT_SCHED_GUEST=y` and `CONFIG_PVSCHED_GUEST_PCI`.
`policy=` and `policy-version=` name the host policy the device advertises,
by default the built-in `default`, version 1. QEMU needs access to the
root-only `/dev/pvsched` and must keep `CAP_SYS_NICE` for its whole lifetime,
because every control ioctl checks it, so do not drop privileges after
startup (for example with `-run-with user=`). Each attached page counts
against `RLIMIT_MEMLOCK`, one page per vCPU, unless QEMU has `CAP_IPC_LOCK`.

## Branches

`pvsched-v3-7.2` is upstream `v11.0.4` plus the device as a short patch queue,
rebased onto new releases and force-pushed; `pvsched-v3` is the 7.1 prototype
on `v9.2.0`.
