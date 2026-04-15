#!/bin/bash

script_path=${0%/*}/
src_path=${script_path}../../ESPHamClock/
log_file=../doc/eeprom/hceeprom.log
csv_file=../doc/eeprom/hceeprom.csv
contrib_path=../hamclock-contrib/

cd ${src_path}
perl ${contrib_path}hceeprom.pl -a | grep NV_ > ${log_file}
echo Addr,Name,Len,Type,Description > ${csv_file}
awk '
BEGIN { FS=" " ; OFS="," }
{
    # Extract first 4 fields
    f1 = $1
    f2 = $2
    f3 = $3
    f4 = $4

    # Rebuild field 5 from field 5 to end
    f5 = ""
    for (i=5; i<=NF; i++) {
        if (f5 != "") f5 = f5 " "
        f5 = f5 $i
    }

    # Properly quote field 5 if needed
    if (index(f5, ",") || index(f5, "\"") || index(f5, "\n")) {
        gsub(/"/, "\"\"", f5)
        f5 = "\"" f5 "\""
    }

    print f1, f2, f3, f4, f5
}' ${log_file} >> ${csv_file}
rm ${log_file}
