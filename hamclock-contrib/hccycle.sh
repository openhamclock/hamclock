#!/opt/local/bin/bash
# run forever to slowly cycle through most HamClock pane content.

# clock host:port
HOST=192.168.7.117:8080

# pane choices, adjust as desired
pane_choices=(                                                                         \
    DE_Wx DX_Wx Solar_Flux Planetary_K Moon Space_Wx Sunspot_N X-Ray ENV_Temp          \
    ENV_Press ENV_Humid ENV_DewPt SDO_Comp SDO_6173A SDO_Magneto SDO_193A Solar_Wind   \
)

# choose one at random forever
while true ; do
    pane=$((1 + ($RANDOM % 3)))
    choice=${pane_choices[ $(($RANDOM % ${#pane_choices[@]})) ]}
    # echo $pane $choice
    if ! curl --silent "http://$HOST/set_pane?Pane$pane=$choice" > /dev/null ; then exit; fi
    sleep 60
done
