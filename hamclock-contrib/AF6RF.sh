#!/bin/bash
#
# Developed by C. Myers (AF6RF)
#
# This script rotates Ham Clock Panes 1-3 through a
# set of user selected choices and updates
# the Ham Clock over the web interface.
#
# MAKE SURE THIS SCRIPT IS EXECUTABLE
#   chmod +x hcRotate.sh
#
# This script can be set to run every X minutes with cron.
# Configure cron as follows in linux:
# The following tells cront to run the script every 2 minutes.
#   */2 * * * * /path/to/script/hcRotate.sh
#
 
# Set HOST (IP address) and PORT of hamclock
HOST=hamclock.ntt
PORT=8080
 
# Pane options with indexes, names need to exactly match the web interface
PaneOptions=(\
      [0]='VOACAP' \
      [1]='DE_Wx' \
      [2]='DX_Cluster' \
      [3]='DX_Wx' \
      [4]='Solar_Flux' \
      [5]='Planetary_K' \
      [6]='Moon' \
      [7]='Space_Wx' \
      [8]='Sunspot_N' \
      [9]='X-Ray' \
      [10]='ENV_Temp' \
      [11]='ENV_Press' \
      [12]='ENV_Humid' \
      [13]='ENV_DewPt' \
      [14]='SDO_Comp' \
      [15]='SDO_6173A' \
      [16]='SDO_Magneto' \
      [17]='SDO_193A' \
      [18]='Solar_Wind' \
      )
 
# Pane Choice Indexes
# UPDATE THESE WITH YOUR PERSONAL SELECTION
# Add as many as desired to each pane, with a minimum of 1
# Panes will be rotated in order of entry
Pane1=(1)
Pane2=(5 8)
Pane3=(6 7 17)
 
#####################################
### DO NOT MODIFY BELOW THIS LINE ###
#####################################
 
# variables that get store choice and get updated with each run of script
p1=0;#will be incremented
p2=0;#will be incremented
p3=0;#will be incremented
 
# rotate the choices by incrementing and wrapping
p1_next=$(( (p1 + 1) % ${#Pane1[@]} ))
p2_next=$(( (p2 + 1) % ${#Pane2[@]} ))
p3_next=$(( (p3 + 1) % ${#Pane3[@]} ))
 
# update stored choices - edits this file in place
sed -i "/#will be incremented$/s/p1=.*#/p1=$p1_next;#/" ${0}
sed -i "/#will be incremented$/s/p2=.*#/p2=$p2_next;#/" ${0}
sed -i "/#will be incremented$/s/p3=.*#/p3=$p3_next;#/" ${0}
 
# print out next set of choices
echo "Pane 1:" ${PaneOptions[${Pane1[$p1_next]}]}
echo "Pane 2:" ${PaneOptions[${Pane2[$p2_next]}]}
echo "Pane 3:" ${PaneOptions[${Pane3[$p3_next]}]}
 
# update the panes via web interface
# this will output the web interface results to file: out.txt
curl -o out.txt "http://$HOST:$PORT/set_pane?Pane1=${PaneOptions[${Pane1[$p1_next]}]}"
curl -o out.txt "http://$HOST:$PORT/set_pane?Pane2=${PaneOptions[${Pane2[$p2_next]}]}"
curl -o out.txt "http://$HOST:$PORT/set_pane?Pane3=${PaneOptions[${Pane3[$p3_next]}]}"
