# WireGuard for Linux 3.10 - 5.5

WireGuard was merged into the Linux kernel for 5.6. This repository contains a backport of WireGuard for kernels 3.10 to 5.5, as an out of tree module.

**More information may be found at [WireGuard.com](https://www.wireguard.com/).**

## Installation on CentOS Stream 8, RHEL 8 (and maybe clones)

First, you have to enable the [EPEL](https://docs.fedoraproject.org/en-US/epel/) repository. Then do the following procedure:
```
sudo dnf install git dkms wireguard-tools
sudo systemctl enable dkms
sudo systemctl start dkms
git clone https://github.com/darkranger-red/wireguard-linux-compat.git
cd wireguard-linux-compat/src
sudo make dkms-install
sudo mv /usr/src/wireguard /usr/src/wireguard-1.0.20220627
sudo dkms install -m wireguard -v 1.0.20220627
```

To see if the `wireguard` module has been installed successfully:
```
dkms status
modinfo wireguard
```

## License

This project is released under the [GPLv2](COPYING).
