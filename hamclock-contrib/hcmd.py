#!/usr/bin/python3

#                                  The MIT License (MIT)
# Copyright © 2021, Barry S. Berg, all Rights Reserved
#
# Permission is hereby granted, free of charge, to any person obtaining a copy of this software 
# and associated documentation files (the “Software”), to deal in the Software without restriction, 
# including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, 
# and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, 
# subject to the following conditions:
#
# The above copyright notice and this permission notice shalfrom urllib.request import urlopen
#
# THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, 
# INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE 
# AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, 
# DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, 
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#
# Note:  This software is offered free to users of the HamClock Software Application and backend service
# Copyright © 2020-2021 Elwood Chales Downey herafter known as HamClock as an accessory piece of software.
# In no form or manner does this License affect or change the terms and conditions set forth
# in the License of HamClock nor will it work without it.  
#  

import datetime
from enum import Enum
import os
import sys
import platform
from io import BytesIO   
import re
import xml.etree.ElementTree as ET

#######################################################################################
#                                                                                     #
#                              Global Variables                                       #
#                                                                                     #
#######################################################################################

ProgName = ''
Version = '1.1'
#Release = '0'
#Release = '0α'
Release = '4β'

MyPlatform = platform.system()
PyMajorVersion = sys.version_info.major
PyMinorVersion = sys.version_info.minor

# Configuration File
if(MyPlatform == "Windows"):
    ConfigFilePath =os.system('echo %USERPROFILE%') + '\\AppData\\Local\\' 
    ConfigCmd = "move -y "  
else:
    ConfigFilePath = os.path.expanduser('~') + '/'
    ConfigCmd = "mv "

# Preserve Previous Configuration Files
ConfigFile = ConfigFilePath + 'hccmd.txt'
OldConfig = ConfigFilePath + 'HCcmd.txt'
OlderConfig = ConfigFilePath + 'HCcmd.cfg '

# Temporary File for scratch Pad
RSS = ConfigFilePath + 'solarrss.php'

if(os.path.isfile(OlderConfig)):
    os.system(ConfigCmd + OlderConfig + ConfigFile)
elif(os.path.isfile(OldConfig)):
    os.system(ConfigCmd + OldConfig + ConfigFile)
elif(os.path.isfile(RSS)):
    os.system('rm -vf ' + RSS)

#Python3 Setup Links

# MacOS Python Setup
MacOSlink = "https://www.digitalocean.com/community/tutorials/how-to-install-python-3-and-set-up-a-local-programming-environment-on-macos"
# Linux (CENTOS RaspianOS, or Debian)
LinuxLink = "https://www.digitalocean.com/community/tutorials/how-to-install-python-3-and-set-up-a-local-programming-environment-on-ubuntu-16-04"
# Windows
Win10Link = "https://www.digitalocean.com/community/tutorials/how-to-install-python-3-and-set-up-a-local-programming-environment-on-windows-10"
os.system("clear")

# NOAA Solar Prediction Link
SolarLink = "https://forecast.weather.gov/product.php?site=NWS&issuedby=PRE&product=DAY&format=txt&version=1&glossary=0"

Host = ''
MyCall = ''
User = ''
Passwd = ''

#######################################################################################
#                                                                                     #
#                  Constants -- Python does not have Constants                        #
#               so this is the Funky way to define them as a class                    #
#                                                                                     #
#######################################################################################

# Funky way to have Constants in Python
class c():   # Cross Platform Character Colors
# Light Colors
    BLACK =     '\33[30m'
    DkRED =     '\33[31m'
    GREEN =     '\33[32m'
    YELLOW =    '\33[33m'
    BLUE   =    '\33[34m'
    VIOLET =    '\33[35m'
    CYAN =      '\33[36m'
    WHITE  =    '\33[37m'
    DEFAULT =   '\33[39m'
# Dark Colors
    GREY =      '\33[90m'
    RED =       '\33[91m'
    DkGREEN =   '\33[92m'
    DkYELLOW =  '\33[93m'
    DkBLUE =    '\33[94m'
    DkVIOLET =  '\33[35m'
    DkBEIGE  =  '\33[36m'   
    DkWHITE =   '\33[97m'

class fmt():            # Format Types
    END =       '\33[0m'
    BOLD =      '\33[1m'
    ITALIC =    '\33[3m'
    URL =       '\33[4m'
    BLINK  =    '\33[5m'
    BLINK2 =    '\33[6m'
    SELECT =    '\33[7m'

class bg():             # Background Formats
    BLACK =     '\33[40m'
    RED =       '\33[41m'
    GREEN =     '\33[42m'
    YELLOW =    '\33[43m'
    BLUE =      '\33[44m'
    VIOLET =    '\33[45m'
    CYAN =      '\33[46m'
    WHITE =     '\33[47m'
    DEFAULT =   '\33[49m'

class Msg(Enum):
    NORM = 0
    ERROR = 1
    WARN = 2
    MESG = 3
    URL = 4
    COMMENT = 5
    COPYRIGHT = 6
    NOTICE = 7
    FCST = 8
    FAIL = 9
                
#######################################################################################
#                                                                                     #
#                           Imports after critical formats defined                    #
#                                                                                     #
#######################################################################################

try:
    import certifi
except Exception as ex:
    sys.exit(c.BLACK + bg.RED + 'Fatal Error: ' + str(ex) + '\nTerminating\n')

try:
    import pycurl
except Exception as ex:
    print(c.BLACK + bg.RED + 'Fatal Error: ' + str(ex) + fmt.END)
    
try:
    import urllib3
except Exception as ex:
    sys.exit(c.BLACK + bg.RED + 'Fatal Error: ' + str(ex) + '\nTerminating\n')
 
urllib3.disable_warnings()

Header = 'http://'
Port = ":8080/"
Band = ''
Action = ''

#######################################################################################
#                                                                                     #
#                                Global Dictionaries                                  #
#                                                                                     #
#######################################################################################

Initial = {'MYCALL':'', 'DE_lat':'', 'DE_lng':'', 'DX_lat':'', 'DX_lng':'', 'Pane1':'', 'Pane2':'', 'Pane3':''}

SpaceWx = {'SSN':'', 'KP':'', 'AP0':'', 'AP1':'', 'AP2':'', 'FLUX':'', 'XRAY':'', 'SOLWIND':'', 'DEDX_80m':'', 'DEDX_40m':'',
    'DEDX_30m':'', 'DEDX_20m':'', 'DEDX_17m':'', 'DEDX_15m':'', 'DEDX_12m':'', 'DEDX_10m':'', 'R0':'', 'R1':'', 'R2':'', 'R3':'',
    'S0':'', 'S1':'', 'S2':'', 'S3':'', 'G0':'', 'G1':'', 'G2':'', 'G3':'', }

SolarElements = {'updated':'', 'solarflux':'', 'aindex':'', 'kindex':'', 'kindexnt':'', 'xray':'', 'sunspots':'', 'heliumline':'', 
    'protonflux':'', 'electronflux':'', 'aurora':'', 'normalization':'', 'latdegree':'', 'solarwind':'', 'magneticfield':''}

Propagation = {'80m-40m-day':'', '80m-40m-night':'', '30m-20m-day':'', '30m-20m-night':'', '17m-15m-day':'', '17m-15m-night':'', 
    '12m-10m-day':'', '12m-10m-night':''}

CalculatedVHFConditions = {'vhf-aurora-northern_hemi':'', 'E-Skip-europe':'', 'E-Skip-north_america':'', 'E-Skip-europe_6m':'', 
    'E-Skip-europe_4m':''}

#######################################################################################
#                                                                                     #
#          The following Sets are used for Input Validation                           #
#                                                                                     #
#######################################################################################

ValidBand = set(['80', '40', '30', '20', '17', '15', '12', '10','OFF'])
ValidPane = set(['VOACAP','DE_Wx','DX_Cluster','DX_Wx', 'Solar_Flux', 'Planetary_K', 'Moon',
                  'Space_Wx', 'Sunspot_N', 'X-Ray', 'Gimbal,' 'ENV_Temp', 'ENV_Press', 'ENV_Humid',
                   'ENV_DewPt', 'SDO_Comp', 'SDO_6173A', 'SDO_Magneto', 'SDO_193A', 'Solar_Wind'])

#######################################################################################
#                                                                                     #
#                                      Functions                                      #
#                                                                                     #
#######################################################################################


def CurlReq(cmd='help'):

    url = Header + Host + Port + cmd
    return(GetWeb(url))

def WebReq(url):
    return(GetWeb(url))

def GetWeb(url): 

    buf = BytesIO()
    try:
        crl = pycurl.Curl()
    except Exception as ex:
        print(CCfmt(c.BLACK + bg.RED, 'Fatal Error: ' + str(ex)))

    crl.setopt(crl.URL, url)
    crl.setopt(crl.WRITEDATA, buf)

    try:
        crl.perform()
    except Exception as ex:
        s = CCfmt(c.BLACK + bg.RED,'\nFATAL ERROR: Exception when performing CURL:' + str(ex) + '    URL of error: ' + str(url) + '\n TERMINATING \n\n')
        sys.exit(s)
    
    crl.close()
    get_body = buf.getvalue()

    return get_body.decode('utf8')
            # get the Solar factors
def GetKeyValue(lbl, key, d= None):
    if(d == None):
        if lbl.find('\r\n') != -1 :
            delim = '\r\n'

        elif lbl.find(',') != -1 :
            delim = ','
        else :
            delim = '\n'
    else:
        delim = d   # use passed in delimeter

    
    if(delim == ','):                           # Parsing SpaceWx
        start = lbl.find(key)
        start = lbl.find(delim, start)+1
        end = lbl.find(delim, start)
    elif(delim == '\r\n' or delim == '\n'):     # Parsing either Panel or Saving Initial Variables
        start = lbl.find(key) + 10
        end = lbl.find(delim, start)

    return lbl[start:end]

def ReadConfig():
    global  Host, MyCall, User, Passwd
    try:
        FilObj = open(ConfigFile, 'r')
    except Exception as ex:
        print(c.DkRED + 'Error: Reading '+ ConfigFile, end = ' ')
        print(ex + fmt.END)
        sys.exit(-1)
    try:
        data = FilObj.readlines()
    except Exception as ex:
        print(fmt.END + 'Error: ' + ex + fmt.END)
        sys.exit(-1)

    if(len(data) > 0):
        Host = data[0].strip()
        User = data[1].strip()
        Passwd = data[2].strip()
        MyCall = User
        os.environ['QRZ_USER'] = User
        os.environ['QRZ_PASSWORD'] = Passwd
        FilObj.close()
    else:                   # Empty File get data
        GetCfg()

    return

def WriteConfig():
    global Host, MyCall, User, Passwd
    
    if(Host == '' or User == '' or Passwd == ''):  # Did we just do this in read Config?
        GetCfg()                  # Check if Config File Info needs update

    try:                    # open file for writing
        FilObj = open(ConfigFile, 'w+')
    except Exception as ex:
        print(c.YELLOW +'Can not create: ' + ConfigFile + fmt.END + c.DkRED)
        print(ex)
        print(fmt.END)
        sys.exit(-1)
    else:
        FilObj.write(Host + '\n')
        FilObj.write(User + '\n')
        FilObj.write(Passwd +'\n')

    FilObj.close()
    return

#   Get Configuration Information & Set Globals
def GetCfg():
    global Host, MyCall, User, Passwd 

    if(Host == ''):
        Ccons(Msg.ERROR, '\n\nError Encountered')
        Ccons(Msg.WARN, 'No Record where your version of HamClock is running.')
        Ccons(Msg.COMMENT, 'Please enter either the IP address ', ' ')
        Ccons(Msg.MESG, 'xxx.xxx.xxx.xxx', ' ')
        Ccons(Msg.COMMENT, 'or a DNS address, Like:', ' ')
        Ccons(Msg.NOTICE, 'HamClock.local', ' ')
        Ccons(Msg.COMMENT, 'so I can find it.')
        host = input(c.CYAN + " Enter the IP/Host Address of HamClock:  " + fmt.END) 
        Host = host
        Ccons(Msg.COMMENT, 'Saving: ', ' ')
        Ccons(Msg.WARN, Host, ' ')
        Ccons(Msg.COMMENT, 'to Configuration File.\n')

    if(User == ''):
        Ccons(Msg.NOTICE, 'This software will perform Call Sign lookups on QRZ. Which requires a login.')
        Ccons(Msg.COMMENT, 'If you dont have a login please go to: ', ' ')
        Ccons(Msg.URL, 'http://QRZ.com', ' ')
        Ccons(Msg.COMMENT, 'and signup.  Then return here... ')
        Ccons(Msg.COMMENT, 'I can wait here for you, or terminate me and come back later. \n')
        
        Ccons(Msg.COMMENT, 'Your QRZ UserId is usually your ', '')
        Ccons(Msg.NOTICE, 'Call Sign')
        user = input(c.CYAN + "Please enter your QRZ UserId : " + fmt.END)
        os.environ['QRZ_USER'] = user
        User = user

    if(Passwd == ''):
        Passwd = input(c.CYAN + 'Please enter your QRZ password: ' + fmt.END)
        os.environ['QRZ_PASSWORD'] = Passwd

        Ccons(Msg.COMMENT, 'Your UserId: ', '')
        Ccons(Msg.WARN, user, '')
        Ccons(Msg.COMMENT, ' Password: ', '')
        Ccons(Msg.WARN, Passwd, '')
        Ccons(Msg.COMMENT, ' will be saved for future use. ', '')
        WriteConfig()
    return

#   Print Color Console Format Routine
def CCfmt(color, string, switch2 = None):
    output = str(str(color) + string)
    if(switch2 == None):
        return output + fmt.END
    else:
        return output + switch2

def Ccons(typ, string, NR = None):

    if(typ == Msg.ERROR):
        s = CCfmt(c.WHITE + bg.RED, string)
    elif(typ == Msg.FAIL):
        s = CCfmt(c.BLACK + bg.RED, string)
    elif(typ == Msg.WARN):
        s = CCfmt(c.YELLOW + bg.BLACK, string)
    elif(typ == Msg.MESG):
        s = CCfmt(c.GREEN, string)
    elif(typ == Msg.URL):
        s = CCfmt(fmt.URL + c.DkBLUE, string)
    elif(typ == Msg.COMMENT):
        s = CCfmt(c.CYAN, string)
    elif(typ == Msg.COPYRIGHT):
        s = CCfmt(c.BLACK + bg.CYAN, string)
    elif(typ == Msg.NOTICE):
        s = CCfmt(c.YELLOW, string)
    elif(typ == Msg.FCST):
        s = CCfmt(c.BLACK + bg.WHITE, string)
    elif(typ == Msg.NORM):  # Plain Default Console Text Color
        s = CCfmt(c.DEFAULT, string)
    else:                   # Default for noGo to Normal
        s = CCfmt(c.DEFAULT + bg.DEFAULT, string)

    if(NR == None):
        print (s)
    else:
        print(s, end = NR)

    return

def ApFcst(Index):
    i = int(Index)

    if(i < 7):
        s = "Conditions are Very GOOD, 10 - 160 meter bands should be hopping"
    elif(i < 11):
        s = "Conditions are OKAY, Consider 30, 40, 60, 80, 160 meter band"
    elif(i > 10):
        s = "Conditions are VERY POOR, Propagation is likely Awful"
    s = s + ", use Bands and this as Major, the following as minor:"
    return(s)

def GeoMagFcst(Index):
    i = int(Index)                   # A case statement would be nice here
    if(i == 0):
        s = "STABLE: Conditions Favor Excellent HF, 10 - 160 meter bands should be hopping"
    elif(i == 1):
        s = "MINOR: Migratory Annimals Affected, \n"
        s = s + "Conditions Favor Very Good HF. Signals 10-20m begin to be inconsistant, 30-160m bands may yield better results\n "
        s = s + "Aurora may be seen at high LAT 60ºN MLAT, typ Nor. MI & MN, ME & Canada"
    elif(i == 2):
        s = "MODERATE: Migratory Annimals Affected, \n"
        s = s + "Conditions Favor Fading of HF, 10-20m are DEGRADING, 30-160 still GOOD\n"
        s = s + "Aurora might be seen as low as 55 ºN MLAT, typ NY & ID"
    elif(i == 3):
        s = "STRONG: Migratory Annimals Affected, \n"
        s = s + "Conditions Favor Intermittent HF, 10-20m are POOR, 30-160 DEGRADING\n" 
    elif(i == 4):
        s = "SEVERE: Migratory Annimals Affected, \n"
        s = s + "Conditions Favor Sporadic HF, 10-20m are VERY POOR, 30-160 POOR\n" 
        s = s + "Aurora may be seen as low as 45ºN MLAT, typ AL & Nor. CA"

    elif(i == 5):
        s = "EXTREME: Migratory Annimals Affected, \n"
        s = s + "Conditions Favor HF Radio IMPOSSIBLE for 1 or 2 Days\n" 
        s = s + "Aurora may be seen as low as 40ºN MLAT, Typ FL & So. TX"
 
    return(s)

def SolRadFcst(Index):
    i = int(Index)

    if(i == 0):
        s = "STABLE: Cond            # get the Solar factorsitions Favor Excellent HF, 10 - 160 meter bands should be hopping"
    elif(i == 1):
        s = "MINOR: Conditions Favor Excellect HF, Minor impacts in the polar regions."
    elif(i == 2):
        s = "MODERATE: Conditions Favor HF Operations OKAY, DEGRADED Effects in the polar regions."
    elif(i == 3):
        s = "STRONG: Conditions Favor DEGRADED HF Operations in the polar regions."
    elif(id == 4):
        s = "SEVERE: Conditions Favor Blackout of HF Operations in the polar regions."
    elif(i == 5):
        s = "EXTREME: Complete Blackout of HF Operations in the polar regions."
    return(s)

def RadBlkoutFcst(Index):
    i = int(Index)

    if(i == 0):
        s = "STABLE: Conditions Favor Excellent HF, 10 - 160 meter bands should be hopping"
    elif(i == 1):
        s = "MINOR: Weak or minor degradation of HF radio communication on sunlit side, occasional loss of radio contact."
    elif(i == 2):
        s = "MODERATE: Limited blackout of HF radio communication on sunlit side, loss of radio contact for tens of minutes."
    elif(i == 3):
        s = "STRONG: Wide area blackout of HF radio communication, loss of radio contact for about an hour on sunlit side of Earth."
    elif(id == 4):
        s = "SEVERE: HF radio communication blackout on most of the sunlit side of Earth for one to two hours. HF radio contact lost during this time."
    elif(i == 5):
        s = "EXTREME: Complete HF (high frequency) radio blackout on the entire sunlit side of the Earth lasting for a number of hours. This results in no HF radio contact with mariners and en route aviators in this sector."
    return(s)

#######################################################################################
#                                                                                     #
#                                   Main Program                                      #
#                                                                                     #
#######################################################################################

os.system("clear")
ProgName = os.path.basename(sys.argv[0])
OpenID = '   ' + ProgName + " (" + Version + "." + Release +")  "
CopyLeft = "Copyright 2021 © Barry S. Berg, -All Rights Reserved-    "
Ccons(Msg.COPYRIGHT, OpenID, '')
Ccons(Msg.COPYRIGHT, CopyLeft)
Ccons(Msg.MESG, '               For Help enter: ' + ProgName + ' HELP')

Ccons(Msg.COMMENT,'Checking Python Version...', ' ')
if (PyMajorVersion <= 3 and PyMinorVersion < 7):
    Ccons(Msg.WARN, '\n=======================================================================')
    s = "You are using Python {}.{}.".format(sys.version_info.major, sys.version_info.minor)
    Ccons(Msg.WARN, "Python 3.7 or higher is required. ", '')
    Ccons(Msg.WARN, s + '            ')
    Ccons(Msg.WARN, '=======================================================================\n')

    Ccons(Msg.NOTICE, 'If you need help, read the article below "How to install Python3 on your system"')
    Ccons(Msg.NOTICE, '  and with Python 3.9 or higher, get the latest homebrew, & pip: \n')
    Ccons(Msg.NOTICE, 'Follow this Link if you need help: \n')
    if(MyPlatform == 'Darwin'):
        Ccons(Msg.URL, MacOSlink + '\n')
        sys.exit(CCfmt(c.BLACK + bg.RED, 'Fatal Error, Terminating Program.\n'))
    elif (MyPlatform == "Windows"):
        Ccons(Msg.URL, Win10Link + '\n')
        sys.exit(CCfmt(c.BLACK + bg.RED, 'Fatal Error, Terminating Program.\n'))
    elif (MyPlatform == "Linux"):
        Ccons(Msg.URL, LinuxLink + '\n')
        sys.exit(CCfmt(c.BLACK + bg.RED, 'Fatal Error, Terminating Program.\n'))
    else:
        Ccons(Msg.WARN, "I can't identify what your system is, so go to Python.org for help     ")
        Ccons(Msg.WARN, '  Download the latest Python3 ver 3.9 or higher                        ')
        Ccons(Msg.WARN, '  you will most likely need the latest versions of homebrew & pip      ')
        Ccons(Msg.WARN, '  two utility programs for loading packages                            ')
        sys.exit(CCfmt(c.BLACK + bg.RED, 'Fatal Error, Terminating Program.\n'))

Ccons(Msg.MESG, ' Version OK.')
# now get the correct version to import call sign info
try:
    from qrz import QRZ
except ImportError:
    Ccons(Msg.WARN, 'You are missing an important module "pyQRZ"                             ')
    Ccons(Msg.WARN, 'I will try and get it for you, but the program pip must be installed.   ')
    if(MyPlatform == "Windows"):
        Ccons(Msg.WARN, 'After you have Python3 ver 3.9 or greater installed then run:           ')
        Ccons(Msg.WARN, '    pip3 install -U pyQRZ as an administrator     WebReq                      ')
        sys.exit(CCfmt(c.DkRED, 'See you soon ...              [Exiting]'))
    else:
        Ccons(Msg.WARN, 'If I error off, look up how to install the program pip on your system,  ')
        Ccons(Msg.WARN, '  if necessary. After it is installed, execute the command line command:')
        Ccons(Msg.WARN, '         sudo -H pip3 install -U pyQRZ                                  ')
        os.system('pip3 install -U PYQRZ')
        sys.exit(CCfmt(c.BLACK + bg.RED, 'Fatal Error, rerun hccmd.py again.'))

if(os.path.isfile(ConfigFile)): 
    Ccons(Msg, "Getting Configuration", " ")
    ReadConfig()    #  read Configuration File
    Ccons(Msg.MESG, ' Done.')
else:               # Create a Configuration File
    WriteConfig()    #  get info and write file

def GetHelp():
    s = str(set(ValidPane)).replace("'", "").replace('{', '').replace('}','').replace(',','|').replace(' ','')

    print(c.YELLOW + "HCcmd [cmd], [value]\n")
    print("HCcmd [cmd], [value], [cmd], [value]...")
    print()
    print("Commands         Values (case may be important)" + fmt.END)
    print("  HELP            This information . . .")
    print("  HC_HELP         Print the HamClock Help page . . .")
    print("  RESET           " + c.DkRED + "Delete the current Stored Configuration and start over" + fmt.END)
    print("  HOST            <host addr> set host address for this run and saves in config") 
    print("                    file for future")
    print("  BAND            80|40|30|20|17|15|12|10|off" + fmt.END)
    print("  CALL            Call Sign -- uses QRZ DB to set DX to QTH of record")
    print("                    " + c.GREEN + "WWV -- sets DX QTH to WWV (Boulder, CO) ")
    print("                     -- for VOACAP of WWV" + fmt.END)
    print(c.YELLOW)
    print("                  Note:  To use QRZ Database you need at least a login to their site, ")
    print("                          and most likely just a login to their site")
    print("                  your login ID & Password are stored in the program config file as well")
    print(fmt.END)
    print("                   Note:  This address will be stored on Disk ")
    print("                     -- Only need to enter once or to change")
    print("  MYCALL           Call Sign -- sets DE to Call Sign QTH")
    print(c.GREEN + "  Pane             Values         Set up display Panels" + fmt.END)
    print("  PANE[1,2,3]      " + s + fmt.END)
    print('')
    print("  Prop             Prints a summary of current Propagation Parameters")
 
    return()

######################################################
#           Parse Command Line                       #
######################################################

if len(sys.argv) < 2:
#    print(sys.argv)
    print("\nUsage: " + ProgName + "<Command> <value>, <Command> <value>")
    GetHelp()
    sys.exit(1)
else:
    if __name__ == "__main__":
        for i, arg in enumerate(sys.argv):
            if i > 0:
#                print(f"Argument {i:>6}: {arg}")
#                print ({arg})
                if arg.upper() == 'HELP':
                    GetHelp()

                elif(arg.upper() == 'RESET'):
                    try:
                        FilObj = open(ConfigFile, 'w')
                     
                    except Exception as ex:
                        print(CCfmt(c.DkRED, 'Error: Could not delete contents of Config File'))
                        sys.exit(CCfmt(c.DkRED, ex))

                    FilObj.close()
                    sys.exit(CCfmt(c.YELLOW, 'Configuration File Cleared.  Re-Run Program again'))

                elif(arg.upper() == 'RESTART'):
                    CurlReq('restart')

                elif (arg.upper() == 'HC_HELP'):
                    print(CurlReq('help'))

                elif arg.upper() == 'HOST':
                    arg = sys.argv[i+1]
                    Host = arg
                    i += 2
                elif arg.upper() == 'BAND':  # set VOACAP Band to arg or off
                    band = sys.argv[i+1]

                    if (band.upper() in ValidBand):
                        CurlReq('set_voacap?Band=' + band)
                    else:
                        print(c.DkRED + 'Error: Invalid Band for VOACAP Setting, must be:' + c.YELLOW)
                        print('      off,80, 40, 30, 20, 17, 15, 12, or 10')
                        sys.exit(c.DkRED +'Exiting!' + fmt.END)

                elif arg.upper() == 'CALL':
                    arg = sys.argv[i+1]
                    Call = arg.upper()
                    i += 3
                    if Call == "WWV":
                        Lat = "40"
                        Lon = "-105"
                    else:
                        certifi.where()
                        qrz = QRZ()
                        Ccons(Msg.MESG, 'Standby Calling ', '')
                        Ccons(Msg.URL, 'http://QRZ.com ', '')
                        Ccons(Msg.MESG, 'for Database: . . .')
                        try:
                            result = qrz.callsign(Call)
                        except ConnectionError:
                            Ccons(Msg.WARN, 'Error: Internet Access Timeout.  Please Try Again.')
                            sys.exit(CCfmt(c.BLACK + bg.RED, '  Exiting without executing command....'))
                        
                        else:
                            if(result['geoloc'] == 'none'):
                                Ccons(Msg.WARN ,'ERROR: Geolocation Information for this Call is Missing.')
                                sys.exit(CCfmt(c.BLACK + bg.RED, '  Exiting without executing command....'))

                            else:
                                Lat = str(int(round(float(result['lat']))))
                                Lon = str(int(round(float(result['lon']))))
                                Grid = result['grid'][:4]

                    s = 'Setting New DX location to ' + Call + ': Lat is ' + Lat +'º Long is ' + Lon + 'º ['+ Grid + ']'
                    Ccons(Msg.MESG, s)
                    CurlReq('set_newdx?lat=' + Lat + '&lng=' + Lon)
                    i += 1

                elif arg.upper()[:4] == "PANE":   #set window panes, now parse pane number and value
                    Num = arg[-1:]
                    Val = sys.argv[i+1]

                    #Not every pane can go into the numbered slots so input validate
                    if(Val not in ValidPane):
                        print(c.DkRED + 'Error: ' + Val + 'Not supported in Pane(s)')
                        print("see help below:")
                        sys.exit('Valid entries are in Pane(s) section\n' + CurlReq('help')+ fmt.END)
    

                    CurlReq('set_pane?Pane' + Num + '=' + Val)

                elif arg.upper() == "MYCALL":
                    User = sys.argv[i+1].upper()
                    MyCall = User
                    Ccons(Msg.NOTICE, 'Note: After this call QRZ will use '+ MyCall + ' for Login ID')
                    certifi.where()
                    qrz = QRZ()

                    Ccons(Msg.MESG, 'Standby Calling ', '')
                    Ccons(Msg.URL, 'http://QRZ.com ', '')
                    Ccons(Msg.MESG, 'for QTH of ' + MyCall + ': . . .')

                    try:
                        result = qrz.callsign(MyCall)
                    
                    except ConnectionError:
                        Ccons(Msg.WARN, 'Error: Internet Access Timeout.  Please Try Again.')
                        sys.exit(CCfmt(c.BLACK + bg.RED, '  Exiting without executing command....'))
 
                    if(result['geoloc'] == 'none'):
                        print(c.YELLOW + 'ERROR: Geolocation Information for this Call is Missing.')
                        sys.exit('       Exiting without executing command....' + fmt.END)

                    Lat = str(int(round(float(result['lat']))))
                    Lon = str(int(round(float(result['lon']))))
                    Grid = result['grid'][:4]
                    CurlReq('set_newde?lat=' + Lat + '&lng=' + Lon)
                    CurlReq('set_pane?Pane3=' + Initial['Pane3'])

                elif arg.upper() == "PROP":
# for testing remove ...
#                    CurlReq('set_pane?Pane1=DE_Wx')
#                    CurlReq('set_pane?Pane2=Moon')
#                    CurlReq('set_pane?Pane3=SDO_Comp')
# for testing remove above ...
                    # get current Panel Settings
                    resp = str(CurlReq('get_config.txt'))
                    Initial.update({'Pane1':GetKeyValue(resp, "Pane1")})
                    Initial.update({'Pane2':GetKeyValue(resp, "Pane2")})
                    Initial.update({'Pane3':GetKeyValue(resp, "Pane3")})

                    resp = str(CurlReq('get_dx.txt'))
                    Initial.update({'DX_lng':GetKeyValue(resp, "DX_lng").strip('degs').strip()})
                    Initial.update({'DX_lat':GetKeyValue(resp, "DX_lat").strip('degs').strip()})
                    
                    resp = str(CurlReq('get_de.txt'))
                    Initial.update({'DE_lng':GetKeyValue(resp, "DE_lng").strip('degs').strip()})
                    Initial.update({'DE_lat':GetKeyValue(resp, "DE_lat").strip('degs').strip()})
                    
                    # get the station Lat/Lon and reverse the signs to find the antipodal point
                    AntiLat = str(int(round(float(Initial.get('DE_lat'))* -1.0)))
                    lng = float(Initial.get('DE_lng'))
                    if(lng < 0.0):
                        lng += 180.0
                    else:
                        lng -= 180.0
                        
                    AntiLon = str(int(round(lng)))

#                     # set the DX to Antipodal point so full path can be computed
# #                    CurlReq('set_newdx?lat='+AntiLat+'&lng='+AntiLon)

#                     # now reset Panes to N_sunspots, xray, KP,Solar_Wind
#                     CurlReq('set_pane?Pane1=Sunspot_N')
#                     CurlReq('set_pane?Pane2=X-Ray')
#                     CurlReq('set_pane?Pane3=Planetary_K')
#                     # now set panes to Solar Flux, VOACAP, NOAA_SpaceWx
#                     CurlReq('set_pane?Pane1=Solar_Flux')
#                     CurlReq('set_pane?Pane2=VOACAP')
#                     CurlReq('set_pane?Pane3=Space_Wx')

#                     # now set panes to VOACAP, NOAA_SpaceWx, Solar_Wind
#                     CurlReq('set_pane?Pane1=Solar_Wind')
#                     CurlReq('set_pane?Pane2=VOACAP')
                    CurlReq('set_pane?Pane3=Space_Wx')

# #                Reset The Panes to Initial
#                     CurlReq('set_pane?Pane1=' + Initial['Pane1'])
#                     CurlReq('set_pane?Pane2=' + Initial['Pane2'])
                    CurlReq('set_pane?Pane3=' + Initial['Pane3'])
# #                Now get the Space Wx Report
#                     # because we set all the panes immediately, the time since last report wil be < 60 secs
#                     # the purpose of the following block of code is to parse the output into a string of
#                     # comma delimited key value pairs.  Ideally if it were to be used outside of this program
#                     # it would be parsed into <key1>:<value1>,<key2>:<value2> ...   However, for simplicity 
#                     # of the code its parsed as <key1>,<value1>,<key2>,<value2> ... as long as there are no null
#                     # keys or values this will work just fine.  However, the order of the instructions below matter
#                     # in case a proper key:value pair is required in the future
#             
            # get the all the Solar Indexes
                    os.system('curl -s -o ' + RSS + ' https://www.hamqsl.com/solarrss.php > /dev/null')

            # Okay get the Solar Data                    
                    tree = ET.parse(RSS)
                    root = tree.getroot()

                    for k, v in SolarElements.items():
#                        print(k)
                        SolarElements.update({k:root.findtext(".//" + k)})
                    
                    for band in root.findall('.//band'):
                        Band = band.get('name')
                        Time = band.get('time')
                        key = str(Band + '-' + Time)
                        value = band.text
                        Propagation.update({key:value})

                    for phen in root.findall('.//phenomenon'):
                        Type = phen.get('name')
                        Loc = phen.get('location')
                        key = str(Type + '-' + Loc)
                        value = phen.text
                        CalculatedVHFConditions.update({key:value})

                    os.system('rm -f ' + RSS)

                    #  value trailing spaces & two digit seconds from the curl request
                    resp = re.sub("   \d{2} secs\n", ",", CurlReq('get_spacewx.txt'))
                    #  value trailing spaces & single digit seconds from the curl request
                    resp = re.sub("   \d secs\n", ",", resp)
                    #strip all leading and trailing spaces & replace all embedded spaces with a comma
                    resp = re.sub("\s+", ",", resp.strip())
                    SpaceWx.update({'R0':GetKeyValue(resp, "NSPW_R0")})
                    SpaceWx.update({'R1':GetKeyValue(resp, "NSPW_R1")})
                    SpaceWx.update({'R2':GetKeyValue(resp, "NSPW_R2")})
                    SpaceWx.update({'R3':GetKeyValue(resp, "NSPW_R3")})
                    SpaceWx.update({'S0':GetKeyValue(resp, "NSPW_S0")})
                    SpaceWx.update({'S1':GetKeyValue(resp, "NSPW_S1")})
                    SpaceWx.update({'S2':GetKeyValue(resp, "NSPW_S2")})
                    SpaceWx.update({'S3':GetKeyValue(resp, "NSPW_S3")})
                    SpaceWx.update({'G0':GetKeyValue(resp, "NSPW_G0")})
                    SpaceWx.update({'G1':GetKeyValue(resp, "NSPW_G1")})
                    SpaceWx.update({'G2':GetKeyValue(resp, "NSPW_G2")})
                    SpaceWx.update({'G3':GetKeyValue(resp, "NSPW_G3")})

                    # Reset DX to original position
                    CurlReq('set_pane?Pane1=' + Initial['Pane1'])
                    CurlReq('set_newdx?lat='+Initial['DX_lat']+'&lng='+Initial['DX_lng'])                       

                    # Get the A Planetary Index from NOAA Space WX
                    resp = GetKeyValue(WebReq(SolarLink), 'A_Planetary ', '\n').lstrip('y').strip()
                    resp = re.sub("\s+", " ", resp.strip())
                    val = list(map(int, resp.split()))
                    SpaceWx.update({'AP0':val[0]})
                    SpaceWx.update({'AP1':val[1]})
                    SpaceWx.update({'AP2':val[2]}) 

#                Now start the analizing the numbers

#                    FFlux = int(round(float(SpaceWx['FLUX'])))
                    FFlux = int(round(float(SolarElements["solarflux"])))
                    Kp = int(round(float(SolarElements['kindex'])))
                    Ap = int(round(float(SolarElements['aindex'])))
                    Ap0 = int(SpaceWx['AP0'])
                    Ap1 = int(SpaceWx['AP1'])
                    Ap2 = int(SpaceWx['AP2'])

                    # Set up Forecast Values
                    GeoMagFcst = []
                    SolRadFcst = []
                    RadBlkoutFcst = []

                    for i in range(4):
                        gk = 'G' + str(i)
                        sk = 'S' + str(i)
                        rk = 'R' + str(i)
                        GeoMagFcst.append(str(SpaceWx[gk]))
                        SolRadFcst.append(str(SpaceWx[sk]))
                        RadBlkoutFcst.append(str(SpaceWx[rk]))

                    Storm = "Indeterminate"
                    if((Kp == 0 or Kp == 1) and Ap < 7):
                        Storm = "GeoMagnetic or Ionospheric conditions are Quiet, So that Favors HF Conditions"
                    elif((Kp == 2 or Kp < 4) and Ap < 30):
                        Storm = "GeoMagnetic or Ionospheric Conditions are Unsettled, So HF conditions are/trending worse" 
                    elif((Kp > 3) and Ap < 50):
                        Storm = 'GeoMagnetic or Ionospheric Conditions are Active, Expect Poor HF Conditions'
                    elif((Kp == 5 or Kp == 6)):
                        Storm = 'GeoMagnaetic or Ionospheric Conditions indicate a Major Storm, Expect Very Poor HF Conditions'
                    elif((Kp == 7) and Ap < 209):
                        Storm = 'GeoMagnetic or Ionospheric Conditions indicate a Severe Storm, HF Conditions will be negatively impacted'
                    elif((Kp == 8 or Kp < 11) and Ap > 300):
                        Storm = 'Major Geomatinetic or Ionospheric Storm, HF Blackout extremely Likely'

                    if (FFlux > 99):
                        FCond = "Conditions are BEST, Favors DX, Lots of Energy to F2 Region"
                    elif (90 <= FFlux <= 99):
                        FCond = "Conditions are VERY GOOD, DX is Better, F2 Layer should be in good shape"
                    elif ((80 <= FFlux <= 89)):
                        FCond = "Conditions are GOOD, DX is just Okay, F2 Layer is expected to be about average"
                    elif ((70 <= FFlux <= 79)):
                        FCond = "Conditions are POOR, DX is Marginal, Don't Expect good Band Conditions"
                    elif (FFlux < 70):
                        FCond = "Conditions are VERY POOR, DX likely Awful, Time to try out that NVIS Antenna"
                   
                    print('SOLAR FLUX  -- Higher is GOOD -- Higher the Number the Higher the Frequency')
                    print('Solar Flux rarely affects 30m-160m bands, Influences 10m-20m Bands')
                    print('The Solar Flux Index is ' + str(FFlux) + ': ' + FCond +'\n')

                    print('Kp and Ap also Affect propagation and Ap has more Influence and both affect Solar Flux')
                    if(Kp > 4):
                        KCond = "Conditions are VERY VERY POOR, Propagation is likely Awful."
                    elif(Kp == 3 or  Kp == 4):
                        KCond = "Conditions are BAD, Consider perhaps 30m-160m band."
                    elif(Kp == 3 or Kp == 2):
                        KCond = "Conditions are OKAY -- 10m-20m bands show promise."
                    elif(Kp < 2):
                        KCond = "Conditions are Very GOOD, 10m-160m bands should be hopping."
                   
                    print('Planetary K Index  -- Lower is GOOD,  Lower the index the Higher the Frequency')
                    print('Kp represents the overall Geo Magnetic Condition of the Ionosphere, of the planet in the past 3 Hours')
                    print('Ap is the Planetary Average and Ap & Kp taken together are indicators of Storms')
                    print('Generally a low and steady Kp generally means good operation on 30m-160m bands')
                    print('The Kp index directly affects propagation the 20m-10m bands.\n')    
                    print('The Planetary K Index is ' + str(Kp) + ': ' + KCond)
                    print('The Planetary A Index is ' + str(Ap)+ ': ' + Storm + '\n')
                    print('Propagation Forecast:')
                    print('Band \t\t\tDay\t\tNight')
                    print('80m-40m\t\t\t' + Propagation['80m-40m-day'] + '\t\t' + Propagation['80m-40m-night'])
                    print('30m-20m\t\t\t' + Propagation['30m-20m-day'] + '\t\t' + Propagation['30m-20m-night'])
                    print('17m-15m\t\t\t' + Propagation['17m-15m-day'] + '\t\t' + Propagation['17m-15m-night'])
                    print('12m-10m\t\t\t' + Propagation['12m-10m-day'] + '\t\t' + Propagation['12m-10m-night'])
                    print('\nFour Day Forecast:  (G,S & R Scale 0-5 Intensity)')

                    WD = ["Mon","Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]
                    dow = datetime.datetime.today().weekday()

                    print('Index \t', end='\t')
#                    print('Index \t\t'+WD[dow]+'\t\t'+WD[(dow=+1 % 6)]+'\t\t'+WD[(dow+=2 % 6)]+'\t\t'+WD[(dow+=3 % 6)])
                    for i in range(4):
                        x = (i + dow) % 7
                        if(i == 0):
                            print(WD[x] + ' (Today)', end='\t')
                        elif(i < 3):
                            print(WD[x] + '\t', end='\t')
                        else:
                            print(WD[x])

                    print('Ap    \t\t'+str(Ap)+'\t\t'+str(Ap0)+'\t\t'+str(Ap1)+'\t\t'+str(Ap2))
                    print('GeoMag Storm',end='\t')
                    for i in range(4):
                        if (i < 3):
                            print(GeoMagFcst[i] + '\t', end= '\t')
                        else:
                            print(GeoMagFcst[i])

                    print('Solar Storm', end='\t')
                    for i in range(4):
                        if (i < 3):
                            print(SolRadFcst[i] + '\t', end='\t')
                        else:
                            print(SolRadFcst[i])
                    print('Radio BlkOut', end='\t')
                    for i in range(4):
                        if(i < 3):
                            print(RadBlkoutFcst[i] + '\t', end='\t')
                        else:
                            print(RadBlkoutFcst[i])
                    
  
                else:
                    continue

WriteConfig()
Ccons(Msg.COMMENT, 'Done . . .\n')
quit()
