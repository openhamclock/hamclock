# Running the HamClock Client with Docker or Podman

This is a containerized deployment of the web version of HamClock. It supports both **Docker** and **Podman** (such as on RHEL, CentOS Stream, Fedora, AlmaLinux, and Rocky Linux).

## How to use it

Grab the `manage-hc-docker-<version>.sh` file from the [latest release](https://github.com/openhamclock/hamclock/releases/latest). That file has a version in the name. You can rename it to `manage-hc-docker.sh`, or download the latest release directly:

```bash
TAG=$(curl -s https://api.github.com/repos/openhamclock/hamclock/releases/latest | grep '"tag_name":' | sed -E 's/.*"([^"]+)".*/\1/')
curl -sLo manage-hc-docker.sh "https://github.com/openhamclock/hamclock/releases/download/${TAG}/manage-hc-docker-${TAG}.sh"
chmod +x manage-hc-docker.sh
```

See the commands available with `./manage-hc-docker.sh help` and do an install with `./manage-hc-docker.sh install`.

* **NOTE:** You'll likely want to use the `-b` option to set the backend server (e.g., `-b ohb.hamclock.app`).
* **NOTE:** You can select from the 4 possible sizes with the `-s` option: `800x480`, `1600x960`, `2400x1440`, `3200x1920`.
* **NOTE:** By default, HamClock binds to host ports `8080` (API), `8081` (Live R/W web UI), and `8082` (Read-Only web UI). If any of these ports are already in use on your host, override them during install using `-a <port>`, `-w <port>`, or `-r <port>`.

## Preconfigure it on a first run

The first time you run it, you can preconfigure some of your personal settings. Look for the [config.env.example](https://github.com/openhamclock/hamclock/blob/main/docker/config.env.example) file. Name it `config.env` and put it in the same folder with your `manage-hc-docker.sh`. Edit it as you like and it will pre-configure your HamClock. If you don't use the `config.env`, you'll get the usual setup screen for a fresh install.

* **NOTE:** `config.env` is only read when `hc.settings` does **not** yet exist in the directory. If an initial install attempt fails midway (for example, due to a port conflict) and creates an empty `hc.settings` file, remove `hc.settings` before re-running `install` so your `config.env` is applied.

## Requirements

* **Container Runtime:**
  * **Docker:** Docker Engine (20.10 or newer) and Docker Compose V2 (`docker compose` CLI plugin, v2.0+) — legacy `docker-compose` (V1) is not supported.
  * **Podman (RHEL / CentOS / Fedora):** Podman (4.x/5.x+) with `podman-docker` (provides the `docker` CLI alias) and `podman-compose` (1.x+) installed.
* **`jq`:** Command-line JSON processor.

## Running with Podman (RHEL / CentOS / Fedora / AlmaLinux / Rocky Linux)

When using `podman` and `podman-compose` as a drop-in replacement for Docker, `podman-compose` handles several CLI options differently than Docker Compose V2:
1. `podman-compose` does not implement the `create` subcommand prior to `up -d`.
2. Passing inline YAML via bash process substitution (`-f <(...)`) closes the file descriptor before `podman-compose` finishes reading it; piping via standard input (`-f -`) is required.
3. Podman requires explicit host IPs or bare port numbers (`8080:8080`) rather than empty host IP colons (`:8080:8080`).

`manage-hc-docker.sh` automatically detects Podman / `podman-compose` and handles its CLI differences transparently.

### Rootless vs. Rootful (`sudo`) Podman Note
Podman maintains separate container storage and networks for rootless users (`podman ps`) and root (`sudo podman ps`).
* If you install HamClock as a regular user (without `sudo`), check and manage it without `sudo` (`podman ps`, `podman logs hamclock`).
* Still check `sudo podman ps` if you encounter a port conflict (`address already in use`), as a rootful container may already be bound to port `8080`, `8081`, or `8082`.
* If an installation fails midway, clean up both the leftover container and Podman pod before retrying:
  ```bash
  podman rm -f hamclock 2>/dev/null || true
  podman pod rm -f pod_manage-hc-docker 2>/dev/null || true
  rm -f hc.settings manage-hc-docker.env
  ```

## Windows & WSL

On Windows, you can run this container seamlessly using Docker Desktop or inside a WSL2 environment. If you prefer running the native X11 desktop GUI on Windows rather than the web version in Docker, see the community guide in [`hamclock-contrib/README.md`](../hamclock-contrib/README.md).