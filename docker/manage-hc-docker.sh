#!/bin/bash

MANAGER_HC_VERSION=latest
GITHUB_LATEST_RELEASE_URL="https://api.github.com/repos/komacke/hamclock/releases/latest"

IMAGE_BASE=komacke/hamclock

# Get our directory locations in order
HERE="$(realpath -s "$(dirname "$0")" 2>/dev/null)"
[ -z "$HERE" ] && HERE="$(realpath "$(dirname "$0")")"
THIS="$(basename "$0")"
STARTED_FROM="$PWD"
cd $HERE

DOCKER_PROJECT=${THIS%.*}
DEFAULT_TAG=$MANAGER_HC_VERSION
GIT_TAG=$(git describe --exact-match --tags 2>/dev/null)
GIT_VERSION=$(git rev-parse --short HEAD 2>/dev/null)
CONTAINER=${IMAGE_BASE##*/}
DEFAULT_HTTP_PORT=:80
DEFAULT_EXTERNAL_HTTP_LOG=false
# the following env is the lighttpd env file
DEFAULT_ENV_FILE="$STARTED_FROM/.env"

# the following env is for sticky settings
STICKY_ENV_FILE=$DOCKER_PROJECT.env
REQUEST_DOCKER_PULL=false
RETVAL=0

main() {
    get_sticky_vars

    COMMAND=$1
    case $COMMAND in
        -h|--help|help)
            usage
            ;;
        -v|--version|version)
            manager_version
            ;;
        check-docker)
            is_docker_installed
            ;;
        check-hc-install|check-hamclock-install)
            is_hamclock_installed
            ;;
        install)
            shift && get_compose_opts "$@"
            install_hamclock
            ;;
        upgrade)
            shift && get_compose_opts "$@"
            upgrade_hamclock
            ;;
        full-reset)
            shift && get_compose_opts "$@"
            recreate_hamclock
            ;;
        reset)
            shift && get_compose_opts "$@"
            docker_compose_reset
            ;;
        restart)
            docker_compose_restart
            ;;
        remove)
            remove_hamclock
            ;;
        up)
            shift && get_compose_opts "$@"
            docker_compose_up
            ;;
        down)
            docker_compose_down
            ;;
        generate-docker-compose)
            shift && get_compose_opts "$@"
            generate_docker_compose
            ;;
        add-env-file)
            shift && get_compose_opts "$@"
            copy_env_to_container
            ;;
        upgrade-me)
            upgrade_this_script
            ;;
        *)
            echo "Invalid or missing option. Try using '$THIS help'."
            exit 1
            ;;
    esac

    if [ "$SAVE_STICKY_VARS" == true -a $RETVAL -eq 0 ]; then
        save_sticky_vars
    fi
}

get_compose_opts() {
    while getopts ":p:s:c:t:e:d:l:" opt; do
        case $opt in
            e)
                REQUESTED_ENV_FILE="$OPTARG"
                ;;
            l)
                REQUESTED_EXTERNAL_HTTP_LOG="$OPTARG"
                if [ "$REQUESTED_EXTERNAL_HTTP_LOG" != true -a "$REQUESTED_EXTERNAL_HTTP_LOG" != false ]; then
                    echo "ERROR: -$opt option must be <true|false>"
                    exit 1
                fi
                ;;
            p)
                REQUESTED_HTTP_PORT="$OPTARG"
                ;;
            t)
                REQUESTED_TAG="$OPTARG"
                ;;
            \?) # Handle invalid options
                echo "Command '$COMMAND': Invalid option: -$OPTARG" >&2
                exit 1
                ;;
            :) # Handle options requiring an argument but none provided
                echo "Command '$COMMAND': Option -$OPTARG requires an argument." >&2
                exit 1
                ;;
        esac
    done

    SAVE_STICKY_VARS=true
}

usage () {
    cat<<EOF
$THIS <COMMAND> [options]:
    help: 
            This message

    check-docker:
            checks docker requirements and shows version

    check-hamclock-install:
            checkif Hamclock is installed and report versions

    install [-p <port>] [-t <tag>]
            do a fresh install and optionally provide the version
            -p: set the HTTP port
            -t: set image tag

    upgrade [-p <port>] [-t <tag>]
            upgrade hamclock; defaults to current git tag if there is one. Otherwise you can provide one.
            -p: set the HTTP port (defaults to current setting)
            -t: set image tag

    full-reset [-p <port>] [-t <tag>]: 
            clear out all data and start fresh
            -p: set the HTTP port (defaults to current setting)
            -t: set image tag

    reset:
            resets the Hamclock container to new but does not reset the persistent storage

    restart:
            restarts the Hamclock container. No file contents modified

    up [-p <port>] [-t <tag>]
            start an existing, not-running Hamclock install; defaults to current git tag if there is one. Otherwise you can provide one.
            -p: set the HTTP port (defaults to current setting)
            -t: set image tag

    down
            stop a running Hamclock install

    remove: 
            stop and remove the docker container, docker storage and docker image

    add-env-file [-e <env file>]:
            add .env to Hamclock. Defaults a file named '.env' in your PWD. The
            .env file contains secrets such as api keys for services. If Hamclock
            was already running, it needs to be restarted for the file
            to take effect. See the restart command. See .env.example for more info.
            -e: .env file location

    generate-docker-compose [-p <port>] [-t <tag>]: 
            writes the docker compose file to STDOUT
            -p: set the HTTP port (defaults to current setting)
            -t: set image tag

    upgrade-me:
            downloads the latest tagged version of itself and overwrites itself. Runs
            the new version to confirm it worked. Does an sha256 validation before
            overwriting itself.
EOF
}

manager_version() {
    echo $MANAGER_HC_VERSION
}

get_sticky_vars() {
    if [ -r $STICKY_ENV_FILE ]; then
        source $STICKY_ENV_FILE
    fi
}

save_sticky_vars() {
    cat<<EOF > $STICKY_ENV_FILE
STICKY_HTTP_PORT="$HTTP_PORT"
STICKY_LIGHTTPD_ENV_FILE="$ENV_FILE"
STICKY_EXTERNAL_HTTP_LOG="$ENABLE_EXTERNAL_HTTP_LOG"
EOF
}

upgrade_this_script() {
    CHECK_LATEST_JSON=$(curl -s "$GITHUB_LATEST_RELEASE_URL")

    URL_LATEST_THIS=$(echo "$CHECK_LATEST_JSON" | jq -r ".assets[] | select(.browser_download_url | contains(\"$DOCKER_PROJECT\")) | .browser_download_url")
    DIGEST_LATEST_THIS=$(echo "$CHECK_LATEST_JSON" | jq -r ".assets[] | select(.browser_download_url | contains(\"$DOCKER_PROJECT\")) | .digest")

    URL_LATEST_RELEASE=$(echo "$CHECK_LATEST_JSON" | jq -r '.html_url')
    AVAILABLE_VERSION=$(basename "$URL_LATEST_RELEASE")

    if [ "$AVAILABLE_VERSION" == "$MANAGER_HC_VERSION" ]; then
        echo "$THIS is currently the latest version: '$MANAGER_HC_VERSION'"
        return $RETVAL
    fi
    cat <<EOF
There is a new version: '$AVAILABLE_VERSION'. The version you have is '$MANAGER_HC_VERSION'.

Source and release notes can be found at this URL:

  $URL_LATEST_RELEASE

Would you like to download the latest version of $THIS and overwrite your current copy?
EOF

    DEFAULT_DOIT=y
    read -p "Overwrite? [Y/n]: " DOIT
    DOIT=${DOIT:-$DEFAULT_DOIT}

    echo
    if [ "${DOIT,,}" == y ]; then
        echo "Getting new version ..."
        TMP_MGR_FILE=$(mktemp -p ./)
        curl -sLo $TMP_MGR_FILE $URL_LATEST_THIS
        chmod --reference=$THIS $TMP_MGR_FILE

        DIGEST_FILE=sha256:$(sha256sum $TMP_MGR_FILE | cut -d ' ' -f1)
        if [ "$DIGEST_FILE" == "$DIGEST_LATEST_THIS" ]; then
            echo "Successfully downloaded new version. Let's run it and check its version:"
            echo
            echo "$ ./$THIS"
            mv $TMP_MGR_FILE $THIS
            exec "./$THIS" version
        else
            echo
            echo "ERROR: downloaded file '$TMP_MGR_FILE' seems to be corrupted. Not using it."
            echo "  Expected: '$DIGEST_LATEST_THIS'"
            echo "  Got:      '$DIGEST_FILE'"
            RETVAL=1
        fi
    else
        echo "Because you answered '$DOIT', we won't upgrade and overwrite. 'Y' or 'y' will do the upgrade."
    fi
}

install_hamclock() {
    is_docker_installed >/dev/null || return $?

    echo "Installing Hamclock ..."

    echo "Starting the container ..."
    if docker_compose_up; then
        echo "Container started successfully."
    else
        echo "ERROR: failed to start Hamclock with docker compose up" >&2
        return $RETVAL
    fi
    return $RETVAL
}

is_hamclock_installed() {
    echo "$THIS version: '$MANAGER_HC_VERSION'"

    echo
    echo "Checking for Hamclock source code from git ..."
    if [ -n "$GIT_VERSION" ]; then
        if [ -n "$GIT_TAG" ]; then
            echo "  release: '$GIT_TAG'"
        elif [ -n "$GIT_VERSION" ]; then
            echo "  git hash: '$GIT_VERSION'"
        fi
    else
        echo "  git checkout not found."
    fi
    TAG_FROM_GIT=$(curl -s --connect-timeout 2 "$GITHUB_LATEST_RELEASE_URL" | grep '"tag_name":' | sed -E 's/.*"([^"]+)".*/\1/')
    echo "  Latest release available from GitHub: '$TAG_FROM_GIT'"

    echo
    echo "Checking for docker ..."
    if ! is_docker_installed | sed 's/^/  /'; then
        RETVAL=1
        return $RETVAL
    fi
    echo

    get_current_image_tag
    if [ -z "$CURRENT_TAG" ]; then
        echo
        echo "Hamclock does not appear to be running. Try running '$THIS up'"
        RETVAL=1
        return $RETVAL
    else
        get_current_http_port
        echo "  Hamclock version:      '$CURRENT_TAG'"
        echo "  Docker image:          '$CURRENT_IMAGE_BASE:$CURRENT_TAG'"
        echo "  HTTP PORT in use:      '$CURRENT_HTTP_PORT'"
    fi

    if ! is_container_running; then
        echo
        echo "Hamclock appears to be in a failed state. Try '$THIS up' and look for docker errors."
    fi
}

upgrade_hamclock() {
    is_docker_installed >/dev/null || return $?

    get_current_http_port
    get_current_image_tag

    echo "Upgrading Hamclock ..."

    REQUEST_DOCKER_PULL=true
    echo "Starting the container ..."
    if docker_compose_up; then
        echo "Container started successfully."
    else
        echo "ERROR: failed to start Hamclock with docker compose up"
        return $RETVAL
    fi
    return $RETVAL
}

is_docker_installed() {
    DOCKERD_VERSION=$(docker -v 2>/dev/null)
    DOCKERD_RETVAL=$?
    DOCKER_COMPOSE_VERSION=$(docker compose version 2>/dev/null)
    DOCKER_COMPOSE_RETVAL=$?
    JQ_VERSION=$(jq --version 2>/dev/null)
    JQ_RETVAL=$?

    if [ $DOCKERD_RETVAL -ne 0 ]; then
        echo "ERROR: docker is not installed. Could not find docker." >&2
        RETVAL=$DOCKERD_RETVAL
    elif [ $DOCKER_COMPOSE_RETVAL -ne 0 ]; then
        echo "ERROR: docker compose is not installed but we found docker. Try installing docker compose." >&2
        echo "  docker version found: '$DOCKERD_VERSION'" >&2
        RETVAL=$DOCKER_COMPOSE_RETVAL
    elif [ $JQ_RETVAL -ne 0 ]; then
        echo "ERROR: jq is not installed. Could not find jq." >&2
        RETVAL=$JQ_RETVAL
    else
        echo "$DOCKERD_VERSION"
        echo "$DOCKER_COMPOSE_VERSION"
        echo "$JQ_VERSION"
    fi
    return $RETVAL
}

docker_compose_up() {
    if is_container_running && [ ${FUNCNAME[1]} != upgrade_hamclock ]; then
        echo "Hamclock is already running."
        RETVAL=1
    else
        docker_compose_yml && docker compose -f <(echo "$DOCKER_COMPOSE_YML") create
        RETVAL=$?
        [ $RETVAL -ne 0 ] && return $RETVAL
        if [ -n "$REQUESTED_ENV_FILE" -o -n "$STICKY_LIGHTTPD_ENV_FILE" -o -r "$DEFAULT_ENV_FILE" ]; then
            copy_env_to_container >/dev/null
        fi
        docker compose -f <(echo "$DOCKER_COMPOSE_YML") up -d
        RETVAL=$?
    fi

    return $RETVAL
}

docker_compose_down() {
    docker_compose_yml && docker compose -f <(echo "$DOCKER_COMPOSE_YML") down -v
    RETVAL=$?

    if is_container_exists; then
        RUNNING_PROJECT=$(docker inspect $CONTAINER | jq -r '.[0].Config.Labels."com.docker.compose.project"')
        if [ "$RUNNING_PROJECT" != "$DOCKER_PROJECT" ]; then
            echo "ERROR: this Hamclock was created with a different docker-compsose file. Please run" >&2
            echo "    'docker stop $CONTAINER'" >&2
            echo "    'docker rm $CONTAINER'" >&2
            echo "before running this utility." >&2
        else
            echo "ERROR: Hamclock failed to stop." >&2
        fi
        RETVAL=1
    fi
    
    return $RETVAL
}

docker_compose_reset() {
    get_current_http_port
    get_current_image_tag
    docker_compose_down || return $RETVAL
    docker_compose_up
}

docker_compose_restart() {
    docker restart $CONTAINER
}

generate_docker_compose() {
    docker_compose_yml && echo "$DOCKER_COMPOSE_YML"
}

remove_hamclock() {
    echo "Stopping the container ..."
    if docker_compose_down; then
        echo "Container stopped successfully."
    else
        echo "ERROR: failed to stop Hamclock with docker compose down" >&2
        return $RETVAL
    fi
}

recreate_hamclock() {
    get_current_http_port
    get_current_image_tag

    remove_hamclock || return $RETVAL
    install_hamclock || return $RETVAL
}

copy_env_to_container() {
    if [ -n "$REQUESTED_ENV_FILE" ]; then
        if [[ "$REQUESTED_ENV_FILE" == /* ]]; then
            ENV_FILE="$REQUESTED_ENV_FILE"
        else
            ENV_FILE="$STARTED_FROM/$REQUESTED_ENV_FILE"
        fi
    elif [ -n "$STICKY_LIGHTTPD_ENV_FILE" ]; then
        ENV_FILE="$STICKY_LIGHTTPD_ENV_FILE"
    else
        ENV_FILE="$DEFAULT_ENV_FILE"
    fi

    if is_container_exists; then
        if [ -r "$ENV_FILE" ]; then
            docker cp $ENV_FILE $CONTAINER:/opt/hamclock-backend/.env
        else
            echo "ERROR: ENV file not found: '$(realpath "$ENV_FILE")'" >&2
            RETVAL=1
        fi
    else
        echo "ERROR: the docker container needs to exist for this command." >&2
        echo "Install or start Hamclock first." >&2
        RETVAL=1
    fi

    return $RETVAL
}

is_container_running() {
    docker ps --format '{{.Names}}' | grep -wqs $CONTAINER
    return $?
}

is_container_exists() {
    docker ps -a --format '{{.Names}}' | grep -wqs $CONTAINER
    return $?
}

get_current_http_port() {
    DOCKER_HTTP_PORT=$(docker inspect $CONTAINER 2>/dev/null | jq -r '.[0].HostConfig.PortBindings."8081/tcp"[0].HostPort')
    DOCKER_HTTP_IP=$(docker inspect $CONTAINER 2>/dev/null | jq -r '.[0].HostConfig.PortBindings."8081/tcp"[0].HostIp')
    echo DEBUG: $CONTAINER, $DOCKER_HTTP_PORT, $DOCKER_HTTP_IP
    if [ "$DOCKER_HTTP_PORT" != 'null' ]; then
        if [ "$DOCKER_HTTP_IP" != 'null' ]; then
            CURRENT_HTTP_PORT=$DOCKER_HTTP_IP:$DOCKER_HTTP_PORT
        else
            CURRENT_HTTP_PORT=:$DOCKER_HTTP_PORT
        fi
    fi
}

get_current_image_tag() {
    CURRENT_DOCKER_IMAGE=$(docker inspect $CONTAINER 2>/dev/null | jq -r '.[0].Config.Image')
    if [ "$CURRENT_DOCKER_IMAGE" != 'null' ]; then
        CURRENT_TAG=${CURRENT_DOCKER_IMAGE#*:}
        CURRENT_IMAGE_BASE=${CURRENT_DOCKER_IMAGE%:*}
    fi
}

get_current_pskr_image_tag() {
    CURRENT_PSKR_DOCKER_IMAGE=$(docker inspect pskr-mqtt-cache 2>/dev/null | jq -r '.[0].Config.Image')
    if [ "$CURRENT_PSKR_DOCKER_IMAGE" != 'null' ]; then
        CURRENT_PSKR_TAG=${CURRENT_PSKR_DOCKER_IMAGE#*:}
        CURRENT_PSKR_IMAGE_BASE=${CURRENT_PSKR_DOCKER_IMAGE%:*}
    fi
}

get_current_voacap_image_tag() {
    CURRENT_VOACAP_DOCKER_IMAGE=$(docker inspect voacap-service 2>/dev/null | jq -r '.[0].Config.Image')
    if [ "$CURRENT_VOACAP_DOCKER_IMAGE" != 'null' ]; then
        CURRENT_VOACAP_TAG=${CURRENT_VOACAP_DOCKER_IMAGE#*:}
        CURRENT_VOACAP_IMAGE_BASE=${CURRENT_VOACAP_DOCKER_IMAGE%:*}
    fi
}

determine_http_port() {
    get_current_http_port

    # first precedence
    if [ -n "$REQUESTED_HTTP_PORT" ]; then
        HTTP_PORT=$REQUESTED_HTTP_PORT

    # second precedence
    elif [ -n "$CURRENT_HTTP_PORT" -a "$CURRENT_HTTP_PORT" != ':' ]; then
        HTTP_PORT=$CURRENT_HTTP_PORT

    # third precedence
    elif [ -n "$STICKY_HTTP_PORT" ]; then
        HTTP_PORT=$STICKY_HTTP_PORT

    # fourth precedence
    else
        HTTP_PORT=$DEFAULT_HTTP_PORT

    fi

    # if there was a :, it was probably IP:PORT; otherwise make sure there's a colon for port only
    [[ $HTTP_PORT =~ : ]] || HTTP_PORT=":$HTTP_PORT"
}

determine_http_log() {

    # first precedence
    if [ -n "$REQUESTED_EXTERNAL_HTTP_LOG" ]; then
        ENABLE_EXTERNAL_HTTP_LOG=$REQUESTED_EXTERNAL_HTTP_LOG

    # second precedence
    elif [ -n "$STICKY_EXTERNAL_HTTP_LOG" ]; then
        ENABLE_EXTERNAL_HTTP_LOG=$STICKY_EXTERNAL_HTTP_LOG

    # third precedence
    else
        ENABLE_EXTERNAL_HTTP_LOG=$DEFAULT_EXTERNAL_HTTP_LOG

    fi

    if [ "$ENABLE_EXTERNAL_HTTP_LOG" == true ]; then
        EXTERNAL_HTTP_LOG_MAPPING="- $HERE/logs/lighttpd:/var/log/lighttpd:rw"
        if [ "${FUNCNAME[2]}" == "docker_compose_up" ]; then
            if [ ! -e "$HERE/logs/lighttpd" ]; then
                mkdir -p "$HERE/logs/lighttpd"
            fi
            if [ "$(stat -c '%u' "$HERE/logs/lighttpd" 2>/dev/null)" != "33" ]; then
                # perms need to be set for logrotate to work
                echo
                echo "WARNING: folder '$HERE/logs/lighttpd' needs the following permission:"
                echo "   sudo chown 33 $HERE/logs/lighttpd"
                echo
            fi
        fi
    fi
}

determine_tag() {
    get_current_image_tag

    # first precedence
    if [ -n "$REQUESTED_TAG" ]; then
        TAG=$REQUESTED_TAG
        return
    fi

    # upgrade shouldn't use the current tag unless it's 'latest'. 
    # GIT_TAG would be empty and we'll get DEFAULT_TAG

    # second precedence
    # FUNCNAME is a stack of nested function calls
    if [ -n "$CURRENT_TAG" -a ${FUNCNAME[3]} != upgrade_hamclock ]; then
        TAG=$CURRENT_TAG

    # third precedence
    elif [ -n "$GIT_TAG" ]; then 
        if [ ${FUNCNAME[3]} == upgrade_hamclock -a "$GIT_TAG" != "$MANAGER_HC_VERSION" ]; then
            echo
            echo "WARNING:"
            echo "         You are in a git repository on tag: '$GIT_TAG'"
            echo "         Your version of '$THIS' is: '$MANAGER_HC_VERSION'"
            echo
            echo "Please run upgrade again setting the version with the -t option."
            echo
            return 1
        fi
        TAG=$GIT_TAG

    # forth precedence
    else
        TAG=$DEFAULT_TAG

    fi
}

docker_compose_yml() {
    determine_http_port
    determine_http_log

    determine_tag || return $?
    IMAGE=$IMAGE_BASE:$TAG

    if [ "$TAG" == "$CURRENT_TAG"  -a "$REQUEST_DOCKER_PULL" == true ]; then
        echo "Doing a docker pull of the image before docker compose."
        docker pull $IMAGE
    fi

    # compose file in $DOCKER_COMPOSE_YML
    IFS= DOCKER_COMPOSE_YML=$(
        docker_compose_yml_tmpl | 
            sed "s/__DOCKER_PROJECT__/$DOCKER_PROJECT/" |
            sed "s|__IMAGE__|$IMAGE|" |
            sed "s/__CONTAINER__/$CONTAINER/" |
            sed "s/__HTTP_PORT__/$HTTP_PORT/" |
            sed "s|__EXTERNAL_HTTP_LOG_MAPPING__|$EXTERNAL_HTTP_LOG_MAPPING|" |
    )
}

docker_compose_yml_tmpl() {
    cat<<EOF
name: __DOCKER_PROJECT__
services:
  web:
    env_file: config.env
    environment:
      - UTC_OFFSET=0
      - BACKEND_HOST=ohb.hamclock.app:80
    container_name: __CONTAINER__
    image: __IMAGE__
    restart: unless-stopped
    networks:
      - hamclock
    ports:
      - "192.168.1.1:8080-8082:8080-8082"
      - "[2600:1700:3ec0:83d8::1]:8080-8082:8080-8082"
      - __HTTP_PORT__:8080-8082
    volumes:
      - type: bind
        source: ./eeprom
        target: /root/.hamclock/eeprom
        bind:
          selinux: Z
    healthcheck:
      test: ["CMD", "curl", "-f", "http://localhost:8080/get_time.txt"]
      timeout: "5s"
      start_period: "60s"
    logging:
      options:
        max-size: "10m"
        max-file: "2"

networks:
  hamclock:
    driver: bridge
    name: hamclock
    enable_ipv6: true
    ipam:
     driver: default
     config:
       - subnet: 172.19.0.0/16
    driver_opts:
      com.docker.network.bridge.name: hamclock
EOF
}

main "$@"
exit $RETVAL
