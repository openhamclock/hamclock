# This little python script reads live dx cluster messages from a running HamClock
# and shows the QRZ page for each station. It can run on any pc at the local network
# Requirements:
# 1. a running HamClock with internet connection
# 2. a configured DX Cluster
# 3. adjust the ip-nr below: ...wget <IP-NR> to ip-nr of HamClock
# 4. adjust the time.sleep(15) to your needs
# 5. chmod +x dx.py
# usage: python ./dx.py

import webbrowser
import os
import time
#os.system('wget <IP-NR> ajust below !
os.system('wget 192.168.178.68:8080/get_dxspots.txt -O dxspots.txt -q')

my_file = open("./dxspots.txt","r")
lines = my_file.read().splitlines()
i=0
for line in lines:
    i=i+1
    call = line[9:17]
    if i > 1:
      print(line)
      url = "https://www.qrz.com/lookup/"+call
      webbrowser.open_new_tab(url)
      time.sleep(15)
    elif i == 1:
      print(line)
my_file.close()
