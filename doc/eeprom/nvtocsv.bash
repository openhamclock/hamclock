#!/bin/bash

perl hceeprom.pl -a | grep NV_ >hceeprom.log
echo Addr,Name,Len,Type,Description >hceeprom.csv
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
}' hceeprom.log >>hceeprom.csv


