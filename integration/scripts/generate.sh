#!/usr/bin/env bash
echo processing standards to build integrated code under hamclock mit license model
HERE="$(realpath -s "$(dirname "$0")")"
intscripts="$(realpath -s $HERE)"
intbuild="$(realpath -s $HERE/../build)"
intstandards="$(realpath -s $HERE/../standards)"
inthc="$(realpath -s $HERE/../../ESPHamClock)"
echo integration and testing should be done on files included in $intstandards in standards repo before adding to the copies in this repo 
echo generating code
$intscripts/insert_license.bash $intstandards/voacap.ant.csv      $intscripts/license_csv.txt      $intbuild/voacap.ant.csv
$intscripts/insert_license.bash $intstandards/antenna_lookup.h    $intscripts/license_mit_cpp.txt  $intbuild/antenna_lookup.h
$intscripts/insert_license.bash $intstandards/antenna_lookup.cpp  $intscripts/license_mit_cpp.txt  $intbuild/antenna_lookup.
$intscripts/insert_license.bash $intstandards/gen_antenna_data.sh $intscripts/license_mit_hash.txt $intbuild/gen_antenna_data.sh
echo copying generated scripts to delivery folder
cp $intbuild/voacap.ant.csv      $inthc/voacap.ant.csv
cp $intbuild/gen_antenna_data.sh $inthc/gen_antenna_data.sh
chmod +x $inthc/gen_antenna_data.sh
cp $intbuild/antenna_lookup.h    $inthc/antenna_lookup.h
cp $intbuild/antenna_lookup.cpp  $inthc/antenna_lookup.cpp

