# HamClock Client

[![C++11 web](https://img.shields.io/github/actions/workflow/status/openhamclock/hamclock/compile-web.yml?branch=main&label=C%2B%2B11%20web&logo=cplusplus&style=flat)](https://github.com/openhamclock/hamclock/actions/workflows/compile-web.yml) [![C++11 Pi](https://img.shields.io/github/actions/workflow/status/openhamclock/hamclock/compile-fb0.yml?branch=main&label=C%2B%2B11%20Pi&logo=raspberrypi&style=flat)](https://github.com/openhamclock/hamclock/actions/workflows/compile-fb0.yml)

This repository is the primary source for ongoing maintenance of the HamClock Client,
aka the HamClock "frontend", and often just referred to as "HamClock".

It is intended as a reference implementation for use with backend servers
that are compatible with the original Clear Sky Institute service and the evolving backend standards
being developed by the [Open Hamclock Standards](https://github.com/openhamclock/hamclock-standards) project.

See [doc](./doc/) for all the information and documentation related to installing, using, and developing HamClock.

The HamClock Client was originally created by Clear Sky Institute, and made available under an [MIT License](./LICENSE).
This repository was started with the source code for Clear Sky Institute's HamClock v4.22, the final version they created.

An archive of historical HamClock Client releases up to 4.22 is available at <https://github.com/openhamclock/hamclock-client-archive>.

## Contributing

Bug reports and pull requests are welcome on GitHub at
<https://github.com/openhamclock/hamclock/issues>

## Releases & Docker Builds

For instructions on building local images, setting up SSH signing keys, and triggering releases via GitHub Actions, see [RELEASE.md](RELEASE.md).

## Related

* [HamClock Standards](https://github.com/openhamclock/hamclock-standards) - specifications source
* [HamClock Client](https://github.com/openhamclock/hamclock) - reference frontend implementation source
    * available on [Google Play](https://play.google.com/store/apps/details?id=org.openhamclock.hamclock) and the Amazon Appstore (see [Android documentation](./android/README.md))
    * [Raspberry Pi OS & Debian](./debian/) - pre-built flashable disk images, automated script, and build instructions
    * [Docker](./docker/README.md) - containerized deployment
    * [User Contributions & Community Guides](./hamclock-contrib/README.md) - Windows (WSL2/WSLg), Proxmox LXC, web proxy, and custom scripts
    * [HamClockLauncher](https://github.com/huberthickman/HamClockLauncher) - macOS frontend installer/launcher source
    * also many appliances available for sale, with HamClock pre-installed and automatically maintained
* [HamClock Pi Images](https://github.com/openhamclock/hamclock-pi-image) - ready-to-flash Raspberry Pi images (Desktop, Web, and Framebuffer)
* Open Hamclock Backend project
    * <https://ohb.works/> - main site
    * <https://github.com/openhamclock/open-hamclock-backend> - source
* [hamclock.com](https://hamclock.com) backend
