# Running the HamClock Client with Docker

This is a dockerized deployment of the web version of HamClock.

## How to use it

Grab the `manage-hc-docker-<version>.sh` file from the [latest release](https://github.com/openhamclock/hamclock/releases/latest). That file has a version in the name. You can rename it to `manage-hc-docker.sh`, or download the latest release directly:

```sh
TAG=$(curl -s https://api.github.com/repos/openhamclock/hamclock/releases/latest | grep '"tag_name":' | sed -E 's/.*"([^"]+)".*/\1/')
curl -sLo manage-hc-docker.sh "https://github.com/openhamclock/hamclock/releases/download/${TAG}/manage-hc-docker-${TAG}.sh"
chmod +x manage-hc-docker.sh
```

See the commands available with ```./manage-hc-docker.sh help``` and do an install with ```./manage-hc-docker.sh install```.

NOTE: you'll likely want to use the -b option to set the backend server.\
NOTE: you can select from the 4 possible sizes with the -s option: ```800x480 1600x960 2400x1440 3200x1920```

### Preconfigure it on a first run

The first time you run it, you can preconfigure some of your personal settings. Look for the [config.env.example](https://github.com/openhamclock/hamclock/blob/main/docker/config.env.example) file. Name it config.env and put it in the same folder with your manage-hc-docker.sh. Edit it as you like and it will pre-configure your hamclock. If you don't use the config.env, you'll get the usual setup screen for a fresh install.

## Requirements

* **Docker Engine** (20.10 or newer)
* **Docker Compose V2** (`docker compose` CLI plugin, v2.0+) — legacy `docker-compose` (V1) is not supported
* **`jq`** command-line JSON processor

## Windows & WSL

On Windows, you can run this container seamlessly using Docker Desktop or inside a WSL2 environment. If you prefer running the native X11 desktop GUI on Windows rather than the web version in Docker, see the community guide in [hamclock-contrib/README.md](../hamclock-contrib/README.md).
