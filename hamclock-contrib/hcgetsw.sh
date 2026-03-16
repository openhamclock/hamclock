#!/opt/local/bin/bash
# get one or more space weather datums from HamClock.
# see usage() for instructions.

# default clock host:port -- adjust or use -h
HOST=ecd-imac27.local:8080

# require at least bash V4 for assoc arrays
if [[ $(echo ${BASH_VERSION} | sed -e 's/\..*//') < 4 ]] ; then
    echo require at least bash version 4
    exit 1
fi

# temp file for get_spacewx.txt
SPWXTMP=/tmp/x.$(basename $0)

# build assoc array to map get_spacewx datum names to set_pane names
declare -A PANES=( \
    ["SSN"]="Sunspot_N" \
    ["KP"]="Planetary_K" \
    ["FLUX"]="Solar_Flux" \
    ["XRAY"]="X-Ray" \
    ["SOLWIND"]="Solar_Wind" \
    ["DEDX_80m"]="VOACAP" \
    ["DEDX_40m"]="VOACAP" \
    ["DEDX_30m"]="VOACAP" \
    ["DEDX_20m"]="VOACAP" \
    ["DEDX_17m"]="VOACAP" \
    ["DEDX_15m"]="VOACAP" \
    ["DEDX_12m"]="VOACAP" \
    ["DEDX_10m"]="VOACAP" \
    ["NSPW_R0"]="Space_Wx" \
    ["NSPW_R1"]="Space_Wx" \
    ["NSPW_R2"]="Space_Wx" \
    ["NSPW_R3"]="Space_Wx" \
    ["NSPW_S0"]="Space_Wx" \
    ["NSPW_S1"]="Space_Wx" \
    ["NSPW_S2"]="Space_Wx" \
    ["NSPW_S3"]="Space_Wx" \
    ["NSPW_G0"]="Space_Wx" \
    ["NSPW_G1"]="Space_Wx" \
    ["NSPW_G2"]="Space_Wx" \
    ["NSPW_G3"]="Space_Wx" \
)

# print usage message and exit 1
usage() {
    echo Purpose: retrieve a space weather datum from HamClock
    echo Usage: $(basename $0) [-h CLOCK_IP] datum ...
    echo where datums are from the set
    for p in ${!PANES[@]}; do
        echo "    " $p
    done | sort -u | column 
    exit 1;
}

# print the given space weather datum, use cache value else get fresh
extractSWDatum() {
    # get name of datum
    DATUM=$1;

    # decide which pane we need
    PANENEED=${PANES[$DATUM]}
    if [ -z $PANENEED ] ; then
        echo Unknown datum: $DATUM
        usage
    fi

    # extract the given datum from $SPWXTMP else get fresh
    if grep -q "$DATUM"'.*secs' $SPWXTMP || grep -q "$DATUM"'.*mins' $SPWXTMP ; then
        # already up to date, just pull out the value
        awk "/$DATUM/{print \$2}" $SPWXTMP
    else
        # find current pane selections
        declare -a PANESNOW=($(curl -s "$CURL/get_config.txt" | awk '/Pane[123]/{print $2}' | tr -d \\r))
        PANE1NOW=${PANESNOW[0]}
        PANE2NOW=${PANESNOW[1]}
        PANE3NOW=${PANESNOW[2]}

        # use existing pane if possible
        if [ $PANE1NOW == $PANENEED ] ; then
            # $DATUM is already on pane1 just needs refresh
            curl -s "$CURL/set_pane?Pane1=$PANENEED" > /dev/null
            curl -s "$CURL/get_spacewx.txt" | awk "/$DATUM/{print \$2}"
        elif [ $PANE2NOW == $PANENEED ] ; then
            # $DATUM is already on pane2 just needs refresh
            curl -s "$CURL/set_pane?Pane2=$PANENEED" > /dev/null
            curl -s "$CURL/get_spacewx.txt" | awk "/$DATUM/{print \$2}"
        elif [ $PANE3NOW == $PANENEED ] ; then
            # $DATUM is already on pane2 just needs refresh
            curl -s "$CURL/set_pane?Pane3=$PANENEED" > /dev/null
            curl -s "$CURL/get_spacewx.txt" | awk "/$DATUM/{print \$2}"
        else
            # not found, use pane2 temporarily then restore
            curl -s "$CURL/set_pane?Pane2=$PANENEED" > /dev/null
            curl -s "$CURL/get_spacewx.txt" | awk "/$DATUM/{print \$2}"
            curl -s "$CURL/set_pane?Pane2=$PANE2NOW" > /dev/null
        fi
    fi
}

# crack args
if [[ "$#" < 1 ]]; then usage; fi
if [[ "$1" =~ "-" ]]; then
    if [[ "$1" != "-h" ]]; then usage; fi
    if [[ "$#" < 3 ]]; then usage; fi
    HOST="$2":8080
    shift 2     # remove -h and host from $@
fi

# handy curl prefix
CURL=http://$HOST

# collect all current space weather info, also good connection test 
if ! curl --connect-timeout 5 --silent "$CURL/get_spacewx.txt" > $SPWXTMP ; then
    echo $HOST connection failed
    exit 1
fi

# print desired datum values
for datum in "$@"; do
    extractSWDatum $datum
done

# clean up
rm $SPWXTMP
