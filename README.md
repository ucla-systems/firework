# Firework

Artifact for *Firework: Efficient and Transparent DSM with Hardware-Coherent
CXL Shared Memory* (ASPLOS 2027). One tree holds all three components:

```
glibc/     glibc 2.35 with the partitioned heap/stack address-space layout
kernel/    Linux 6.3 with the Firework page sharing / unsharing subsystem
tests/     Firework user-level runtime, the five evaluated applications,
           and the scripts that run the experiments and plot Figures 6 and 8
```

`./build.sh` builds everything (`./build.sh kernel-install` installs the
kernel; see the header of `build.sh`). The evaluation machine comes with all
components prebuilt and the kernel installed, so reviewers can go straight to
[tests/README.md](tests/README.md) for the basic test and the Figure 6 / Figure 8
evaluation.

## Prerequisites (for building from source)

Tested on Ubuntu 22.04 (gcc 11, cmake 3.22, python 3.10):

```bash
sudo apt install build-essential bc bison flex gawk cpio kmod \
    libssl-dev libelf-dev libncurses-dev \
    cmake ndctl python3 python3-matplotlib
```

- kernel: `build-essential bc bison flex cpio kmod libssl-dev libelf-dev libncurses-dev`
- glibc: `build-essential gawk bison python3`
- runtime and applications: `build-essential cmake` (`g++` comes with `build-essential`)
- running the experiments: `ndctl` (`scripts/setup.sh` reconfigures the DAX
  namespaces), `sudo` (the ranks run as root), `python3-matplotlib` (plots)

Note: the CXL memory must be exposed as two DAX devices (`/dev/dax0.0` on
near CXL memory, `/dev/dax1.0` on far CXL memory), which the evaluation
machine does through `memmap=` boot parameters.
