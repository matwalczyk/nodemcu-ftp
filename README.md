# nodemcu-ftp

Nodemcu firmware use module in C providing a ftp server on the ESP8266

Requires a working build chain or docker image (see https://nodemcu.readthedocs.io/en/master/en/build/).
This module was build with a dedicated tool chain in an OSX build environment and developed in Xcode.

This module implements a simple ftp server in nodemcu on top of the SPIFFS filesystems of the ESP8266.

The FTP server can be started and stopped in the nodemcu LUA environment with ftp.start() and ftp.stop().

When the server is started you can store, retrieve, rename and delete files on the SPIFFS filesystem with any standard ftp client on LINUX, OSX or Windows.

The current implementation main limitations are 
- Only passive mode supported
- Only 1 client connection supported
- Only a subset of RFC 959 FTP server commands implemented (e.g. due to SPIFFS no directory related commands).

List of implemented FTP server commands:
-  USER - Username for Authentication
-  PASS - Password for Authentication
-  RNTO - Rename To (previous RNFR required)
-  QUIT - End of Client Session
-  PASV - Passive Connection management
-  ABOR - Abort
-  DELE - Delete a File
-  LIST - List
-  RETR - Retrieve
-  STOR - Store
-  SIZE - Size of the file
-  TYPE - Data Type
-  RNFR - Rename File From
-  PWD  - Print Current Directory 


The following commands are not implemented because SPIFFS does not support directories (return 501 to client):
-  MKD - Make Directory
-  RMD - Remove a Directory
-  CDUP - Change to Parent Directory
-  CWD - Change Working Directory

For any other command not listed here the server return code will be also 501 ("Syntax Error").

The file needs to be placed into the app/modules directory of the nodemcu firmware tree.

To get this module compiled into your nodemcu firmware build you need to add

  #define LUA_USE_MODULES_FTP

in app/include/user_modules.h.
