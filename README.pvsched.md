# pvsched QEMU fork

This fork carries the **pvsched paravirt-scheduling PCI device** on branch
`pvsched-v3`: upstream `v9.2.0` plus one commit adding
`hw/i386/pvsched.c` (see that commit message for the device's register
interface and semantics). Everything else is untouched upstream QEMU.

The device is the discovery transport between a pvsched guest kernel and the
host kernel's pvsched framework — see the companion repos:
[pvsched/linux](https://github.com/pvsched/linux) (framework, guest driver,
design doc at `Documentation/virt/kvm/pvsched-v3.rst`) and
[pvsched/bpf-policy](https://github.com/pvsched/bpf-policy).

## Build

```sh
sudo apt-get install ninja-build libglib2.0-dev libpixman-1-dev libslirp-dev
./configure --target-list=x86_64-softmmu --enable-slirp
ninja -C build qemu-system-x86_64
```

Only the x86-64 system emulator is needed; `libslirp` is optional for the
device itself but required for `-nic user,...` guest networking, which the
pvsched test recipes use. Incremental rebuilds after touching the device:

```sh
ninja -C build qemu-system-x86_64
```

## Run

The host must run a kernel from pvsched/linux (branch `pvsched-v3`) with
`pvsched.ko` and a policy loaded; the guest kernel needs
`CONFIG_PARAVIRT_SCHED_GUEST=y` + `CONFIG_PVSCHED_GUEST_PCI`.

```sh
build/qemu-system-x86_64 -enable-kvm ... -device pvsched,policy=demo
```

`policy=` names the host policy the device advertises to the guest (e.g.
`demo` for the in-tree module, `bpfdemo` for the BPF policy). QEMU needs
access to `/dev/pvsched` (root-only) and `CAP_SYS_NICE` for the registration
ioctls.

## Pushing changes

The device lives in a single file plus one `meson.build` line, kept as **one
commit on top of a release tag** so the diff against upstream stays trivial
to read and rebase.

```sh
git clone -b pvsched-v3 git@github.com:pvsched/qemu.git
# edit hw/i386/pvsched.c ...
git commit --amend        # fold into the device commit (preferred), or add
                          # follow-up commits for logically separate changes
git push --force-with-lease origin pvsched-v3
```

To move to a newer QEMU release, cherry-pick the device commit onto the new
tag and force-push the branch:

```sh
git fetch https://gitlab.com/qemu-project/qemu.git tag v9.3.0
git checkout -B pvsched-v3 v9.3.0
git cherry-pick <device-commit>
git push --force-with-lease origin pvsched-v3
```

Force-pushes are expected on this branch (it is a patch queue, not history);
`--force-with-lease` always.
