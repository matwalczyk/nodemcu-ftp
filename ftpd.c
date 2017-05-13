//
//  ftpcmd.c
//  nodemcu-firmware 1.5.4.1
//
//  Created by Matthias Walczyk on 10.04.17.
//

#include "module.h"
#include "lauxlib.h"
#include "c_string.h"
#include "c_stdlib.h"
#include "c_stdio.h"

#include "user_interface.h"
#include "platform.h"
#include "espconn.h"
#include "vfs.h"
#include "driver/uart.h"

#define FTP_SERVER_VERSION "Version LUAFTP-"__DATE__
#define FTP_CTRL_PORT       21          // Command port on wich server is listening
#define FTP_DATA_PORT_PASV  49999       // Data port for passive mode
#define FTP_TIME_OUT        5           // Disconnect client after 5 minutes of inactivity
#define FTP_USER  "user"
#define FTP_PASS  "ichbines"
#define FTP_DEBUG

typedef enum {
  NOTCONNECTED = 0,
  USERREQUIRED = 1,
  PASSREQUIRED = 2,
  ACCEPTINGCMD = 3,
  RNTOREQUIRED = 4,
  STORRECEIVED = 5,
  LISTRECEIVED = 6,
  RETRRECEIVED = 7
} cmdStatus_t;

typedef enum {
  NODATACONN   = 0,
  NOCONNECTION = 1,
  CONNECTED    = 2,
} dataStatus_t;

int           fdCurrentFile;
static char   command[5];                 // command sent by client
static char   *parameter;                 // point to begin of parameters sent by client
cmdStatus_t   cmdStatus = NOTCONNECTED;   // status of command connection
dataStatus_t  dataStatus = NODATACONN;
bool          dataTypeBinary = false;
static char   strBuf[120];
vfs_dir       *dirToList = NULL;
int           numberOfFiles = 0;

static struct espconn cmdConn, dataConn;
static esp_tcp cmdConn_tcp, dataConn_tcp;

static void ICACHE_FLASH_ATTR printDbg(const char *fmt, ...);
static void sendCmdConn(const char *fmt, ...);
static void ICACHE_FLASH_ATTR doListFiles();
static void ICACHE_FLASH_ATTR doRetrieve();
static bool ICACHE_FLASH_ATTR processCommand();
static int8_t ICACHE_FLASH_ATTR processLine(char *input);
static void dataConnSentCb(void *arg);
static void dataConnRecvCb(void *arg, char *pusrdata, unsigned short bytesRead);
static void dataConnDisconCb(void *arg);
static void dataConnConnectCb(void *arg);
static void ICACHE_FLASH_ATTR dataConnCreate();
static void ICACHE_FLASH_ATTR dataConnClose();
static void ICACHE_FLASH_ATTR cmdConnRecvCb(void *arg, char *pusrdata, unsigned short length);
static void ICACHE_FLASH_ATTR cmdConnSentCb(void *arg);
static void ICACHE_FLASH_ATTR cmdConnDisconCb(void *arg);
static void ICACHE_FLASH_ATTR cmdConnConnectCb(void *arg);
static void ICACHE_FLASH_ATTR cmdConnCreate();
static void ICACHE_FLASH_ATTR cmdConnClose();

static void ICACHE_FLASH_ATTR printDbg(const char *fmt, ...) {
#ifdef FTP_DEBUG
  static char strBufDbg[120];
  va_list arg;
  va_start(arg, fmt);
  c_vsprintf(strBufDbg, fmt, arg);
  va_end(arg);
  uart0_sendStr(strBufDbg);
#endif
}

static void sendCmdConn(const char *fmt, ...) {
  va_list arg;
  va_start(arg, fmt);
  c_vsprintf(strBuf, fmt, arg);
  strcat(strBuf, "\n");
  va_end(arg);
  printDbg("Sending Response ==> %s", strBuf);
  espconn_sent(&cmdConn, strBuf, strlen(strBuf));
}

static void ICACHE_FLASH_ATTR doListFiles()
{
  numberOfFiles = 0;
  dirToList = vfs_opendir(".");
  if (!dirToList) {
    sendCmdConn("550 Can't open directory %s", ".");
    return;
  }
  printDbg("In List Loop!\n");
  cmdStatus = LISTRECEIVED;
  if (dataStatus == CONNECTED) {
    vfs_item *dirItem = vfs_readdir(dirToList);
    if (dirItem) {
      c_sprintf(strBuf, "%-30s %d\r\n", vfs_item_name(dirItem), vfs_item_size(dirItem));
      espconn_sent(&dataConn, strBuf, strlen(strBuf));
      vfs_closeitem(dirItem);
      printDbg("%-30s %d\r\n", vfs_item_name(dirItem), vfs_item_size(dirItem));
      numberOfFiles++;
    }
  }
}

static void ICACHE_FLASH_ATTR doRetrieve()
{
  printDbg("In Retrieve Loop!\n");
  cmdStatus = RETRRECEIVED;
  if (dataStatus == CONNECTED) {
    sint32_t bytesSent = vfs_read(fdCurrentFile, strBuf, 80);
    if (bytesSent > 0) {
      printDbg("Sending %d bytes on data connection!\n", bytesSent);
      espconn_sent(&dataConn, strBuf, bytesSent);
    }
  }
}

static bool ICACHE_FLASH_ATTR processCommand() {
  
  ////////////////////////////////////////////
  //                                        //
  // PROCESS FULLY IMPLEMENTED FTP COMMANDS //
  //                                        //
  ////////////////////////////////////////////
  
  //
  //  USER - Username for Authentication
  //
  if (cmdStatus == USERREQUIRED) {
    if (strcmp(command, "USER"))
      sendCmdConn("%s", "530 Please login with USER and PASS.");
    else if (strcmp(parameter, FTP_USER))
      sendCmdConn("530 user %s not found", parameter);
    else {
      sendCmdConn("331 User %s accepted, provide password.", parameter);
      cmdStatus = PASSREQUIRED;
      return true;
    }
    return false;
  }
  //
  //  PASS - Password for Authentication
  //
  else if (cmdStatus == PASSREQUIRED) {
    if (strcmp(command, "PASS"))
      sendCmdConn("%s", "530 Please login with USER and PASS.");
    else if (strcmp(parameter, FTP_PASS))
      sendCmdConn("%s", "530 Login incorrect");
    else {
      sendCmdConn("230 User %s logged in.", FTP_USER);
      cmdStatus = ACCEPTINGCMD;
      return true;
    }
    return false;
  }
  //
  //  RNTO - Rename To (previous RNFR required)
  //
  else if (cmdStatus == RNTOREQUIRED) {
    if (strcmp(command, "RNTO"))
      sendCmdConn("%s", "503 Last command was RNFR. RNFR requires a RNTO command afterwards.");
    else {
      cmdStatus = ACCEPTINGCMD;
      if (strlen(parameter) == 0)
        sendCmdConn("%s", "501 file name required for RNTO");
      else {
        int testfd = vfs_open(parameter,"r");
        if (testfd) {
          vfs_close(testfd);
          sendCmdConn("553 File %s already exists", parameter);
        } else {
          if (vfs_rename(strBuf, parameter) == 0) {
            sendCmdConn("250 File successfully renamed or moved");
            return true;
          } else
            sendCmdConn("451 Rename/move failure");
        }
      }
    }
    return false;
  }
  //
  //  QUIT - End of Client Session
  //
  else if (!strcmp(command, "QUIT")) {
    cmdConnClose();
    return false;
  }
  //
  //  PASV - Passive Connection management
  //
  else if (!strcmp(command, "PASV")) {
    struct ip_info pTempIp;
    
    if (dataStatus != NODATACONN)
      dataConnClose();
    dataConnCreate();
    wifi_get_ip_info(SOFTAP_IF, &pTempIp);
    if (pTempIp.ip.addr == 0)
      wifi_get_ip_info(STATION_IF, &pTempIp);
    if (pTempIp.ip.addr != 0)
      sendCmdConn("227 Entering Passive Mode (%d,%d,%d,%d,%d,%d).",
                  IP2STR(&pTempIp.ip), FTP_DATA_PORT_PASV >> 8, FTP_DATA_PORT_PASV & 255);
    return true;
  }
  //
  //  ABOR - Abort
  //
  else if (!strcmp(command, "ABOR")) {
    dataConnClose();
    sendCmdConn("%s", "226 Data connection closed");
    return false;
  }
  //
  //  DELE - Delete a File
  //
  else if (!strcmp(command, "DELE")) {
    if (strlen(parameter) == 0) {
      sendCmdConn("%s", "501 No file name");
    } else {
      int testfd = vfs_open(parameter,"r");
      if (!testfd) {
        sendCmdConn("550 File %s not found", parameter);
      } else {
        vfs_close(testfd);
        if (!vfs_remove(parameter)) {
          sendCmdConn("250 Deleted %s", parameter);
          return true;
        } else
          sendCmdConn("450 Can't delete %s", parameter);
      }
    }
    return false;
  }
  //
  //  LIST - List
  //
  else if (!strcmp(command, "LIST")) {
    printDbg("Heap before doListFiles: %d\n", system_get_free_heap_size());
    if (dataStatus == NODATACONN)
      sendCmdConn("%s", "425 No data connection");
    else {
      if (dataStatus == CONNECTED)
        doListFiles();
      else
        cmdStatus = LISTRECEIVED;
      return true;
    }
    printDbg("Heap after  doListFiles: %d\n", system_get_free_heap_size());
    return false;
  }
  //
  //  RETR - Retrieve
  //
  else if (!strcmp(command, "RETR")) {
    if (strlen(parameter) == 0)
      sendCmdConn("%s", "501 No file name");
    else if (dataStatus == NODATACONN)
      sendCmdConn("%s", "425 No data connection");
    else {
      fdCurrentFile = vfs_open(parameter, "r");
      if (!fdCurrentFile) {
        sendCmdConn("Can't open %s: No such file or directory", parameter);
      } else {
        printDbg("Heap before dataConnCreate: %d\n", system_get_free_heap_size());
        if (dataStatus == CONNECTED)
          doRetrieve();
        else
          cmdStatus = RETRRECEIVED;
      }
      return true;
    }
    return false;
  }
  //
  //  STOR - Store
  //
  else if (!strcmp(command, "STOR")) {
    sint32_t bytesRead;
    if (strlen(parameter) == 0)
      sendCmdConn("%s", "501 No file name");
    else {
      fdCurrentFile = vfs_open(parameter, "w");
      if (!fdCurrentFile) {
        sendCmdConn("451 Can't open/create %s", parameter);
      } else if (dataStatus == NODATACONN) {
        sendCmdConn("%s", "425 No data connection");
        vfs_close(fdCurrentFile);
      } else {
        cmdStatus = STORRECEIVED;
        return true;
      }
    }
    return false;
  }
  //
  //  SIZE - Size of the file
  //
  else if (!strcmp(command, "SIZE")) {
    if (strlen(parameter) == 0)
      sendCmdConn("%s", "501 No file name");
    else {
      int fd = vfs_open(parameter, "r");
      if ((fd) && (vfs_lseek(fd, 0, VFS_SEEK_END))) {
        sendCmdConn("213 %d", vfs_tell(fd));
        vfs_close(fd);
        return true;
      } else {
        sendCmdConn("550 Can't open %s", parameter);
        return false;
      }
    }
  }
  //
  //  TYPE - Data Type
  //
  else if (!strcmp(command, "TYPE")) {
    if (!strcmp(parameter, "A")) {
      sendCmdConn("%s", "200 TYPE is now ASCII");
      dataTypeBinary = false;
    }
    else if (!strcmp(parameter, "I")) {
      sendCmdConn("%s", "200 TYPE is now 8-bit binary");
      dataTypeBinary = true;
    }
    else {
      sendCmdConn("%s", "504 Unknow TYPE");
      return false;
    }
    return true;
  }
  //  RNFR - Rename File From
  //
  else if (!strcmp(command, "RNFR")) {
    if (strlen(parameter) == 0)
      sendCmdConn("%s", "501 file name required for RNFR");
    else {
      int testfd = vfs_open(parameter,"r");
      if (!testfd) {
        sendCmdConn("550 File %s does not exist", parameter);
        return false;
      } else {
        vfs_close(testfd);
        sendCmdConn("350 RNFR accepted - file exists, ready for destination");
        strcpy(strBuf, parameter);
        cmdStatus = RNTOREQUIRED;
        return true;
      }
    }
    return false;
  }
  
  ////////////////////////////////////////////
  //                                        //
  //  PROCESS NON IMPLEMENTED DIR COMMANDS  //
  //                                        //
  ////////////////////////////////////////////
  
  //
  //  MKD - Make Directory
  //
  else if (!strcmp(command, "MKD")) {
    sendCmdConn("501 Directory create not possible (not supported with SPIFFS)");
  }
  //
  //  RMD - Remove a Directory
  //
  else if (!strcmp(command, "RMD")) {
    sendCmdConn("501 Directory remove not possible (not supported with SPIFFS)");
  }
  //
  //  CDUP - Change to Parent Directory
  //
  else if (!strcmp(command, "CDUP")) {
    sendCmdConn("501 Directory change not possible (not supported with SPIFFS)");
  }
  //
  //  CWD - Change Working Directory
  //
  else if (!strcmp(command, "CWD")) {
    sendCmdConn("501 Directory change not possible (not supported with SPIFFS)");
  }
  //
  //  PWD - Print Directory
  //
  else if (!strcmp(command, "PWD")) {
    sendCmdConn("257 \"%s\" is your current location (dirs are not supported with SPIFFS)", ".");
  }
  
  ///////////////////////////////////////////////
  //                                           //
  //  PROCESS NON IMPLEMENTED OTHER COMMANDS   //
  //                                           //
  ///////////////////////////////////////////////
  
  //
  //  MODE - Transfer Mode
  //
  else if (!strcmp(command, "MODE")) {
    sendCmdConn("502 MODE Command not implemented.");
  }
  //
  //  PORT - Data Port
  //
  else if (!strcmp(command, "PORT")) {
    sendCmdConn("502 PORT Command not implemented.");
  }
  //
  //  STRU - File Structure
  //
  else if (!strcmp(command, "STRU")) {
    sendCmdConn("502 STRU Command not implemented.");
  }
  //  MLSD - Lists directory contents if directory is named
  //
  else if (!strcmp(command, "MLSD")) {
    sendCmdConn("502 MLSD Command not implemented.");
  }
  //  NLST - List of file names in a specified directory
  //
  else if (!strcmp(command, "NLST")) {
    sendCmdConn("502 NLST Command not implemented.");
  }
  //  NOOP - No operation
  //
  else if (!strcmp(command, "NOOP")) {
    sendCmdConn("502 NOOP Command not implemented.");
  }
  //  FEAT - Feature list implemented by server
  //
  else if (!strcmp(command, "FEAT")) {
    sendCmdConn("502 FEAT Command not implemented.");
  }
  //  MDTM - Return last-modified time of a file
  //
  else if (!strcmp(command, "MDTM")) {
    sendCmdConn("502 MDTM Command not implemented.");
  }
  //  SITE - File Structure
  //
  else if (!strcmp(command, "SITE")) {
    sendCmdConn("502 SITE Command not implemented.");
  }
  //
  //  Other Unrecognized commands ...
  //
  else
    sendCmdConn("500 Unknown Command %s. Not Implemented.", command);
  return false;
}

static int8_t ICACHE_FLASH_ATTR processLine(char *input) {
  int8_t rc = 1;
  
  c_memset(command, 0, sizeof(command));
  parameter = (char *)(input + strlen(input));
  if (strchr(input, '\n')) *strchr(input, '\n')='\0';
  printDbg("Raw Input ==> %s\n", input);
  if (!strchr(input, ' ')) {
    if ((strlen(input) < 3) || (strlen(input) > 4))
      rc = 0; // Syntax Error
    else
      strncpy(command, input, strlen(input));
  } else {
    *strchr(input, ' ') = '\0';
    if ((strlen(input) < 3) || (strlen(input) > 4))
      rc = 0; // Syntax Error
    else
      strncpy(command, input, strlen(input));
    parameter = (char *)(input + strlen(input));
    while (*(++parameter) == ' ');
  }
  if (rc == 0)
    sendCmdConn("%s", "500 Syntax error");
  for (uint8_t i = 0; i < strlen(command); i++)
    command[i] = toupper(command[i]);
  return (rc);
}

static void dataConnSentCb(void *arg) {
  if (cmdStatus == LISTRECEIVED) {
    vfs_item *dirItem = vfs_readdir(dirToList);
    if (dirItem) {
      c_sprintf(strBuf, "%-30s %d\r\n", vfs_item_name(dirItem), vfs_item_size(dirItem));
      espconn_sent(&dataConn, strBuf, strlen(strBuf));
      printDbg("%-30s %d\r\n", vfs_item_name(dirItem), vfs_item_size(dirItem));
      vfs_closeitem(dirItem);
      numberOfFiles++;
    } else {
      vfs_closedir(dirToList);
      dataConnClose();
      sendCmdConn("226 %d matches total", numberOfFiles);
      cmdStatus = ACCEPTINGCMD;
    }
  } else if (cmdStatus == RETRRECEIVED) {
    sint32_t bytesSent = vfs_read(fdCurrentFile, strBuf, 80);
    if (bytesSent > 0) {
      printDbg("Sending %d bytes on data connection!\n", bytesSent);
      espconn_sent(&dataConn, strBuf, bytesSent);
    } else {
      vfs_close(fdCurrentFile);
      dataConnClose();
      sendCmdConn("226 0 seconds (measured here and unkown), uknown Mbytes per second");
      cmdStatus = ACCEPTINGCMD;
    }
  }
}

static void dataConnRecvCb(void *arg, char *pusrdata, unsigned short bytesRead) {
  printDbg("In dataConnRecvCb\n");
  if (cmdStatus == STORRECEIVED) {
    vfs_write(fdCurrentFile, pusrdata, bytesRead);
    printDbg("In dataConnRecvCb - writing %d bytes\n", bytesRead);
  }
}

static void dataConnDisconCb(void *arg) {
  struct espconn *pCon = arg;
  
  if (cmdStatus == STORRECEIVED) {
    printDbg("In dataConnDisconCb  - Closing down connection\n");
    vfs_close(fdCurrentFile);
    espconn_delete(&dataConn);
    sendCmdConn("%s", "226 File successfully transferred");
    cmdStatus = ACCEPTINGCMD;
    dataStatus = NODATACONN;
  }
  espconn_delete(pCon);
  dataStatus = NOCONNECTION;
}

static void dataConnConnectCb(void *arg) {
  struct espconn *pCon = arg;

  printDbg("in DataConnConnectCallback!\n");
  sendCmdConn("%s", "150 Accepted data connection");
  espconn_regist_recvcb(pCon, dataConnRecvCb);
  espconn_regist_sentcb(pCon, dataConnSentCb);
  espconn_regist_disconcb(pCon, dataConnDisconCb);
  dataStatus = CONNECTED;
  if (cmdStatus == LISTRECEIVED) doListFiles();
  if (cmdStatus == RETRRECEIVED) doRetrieve();
}
  
static void ICACHE_FLASH_ATTR dataConnCreate()
{
  ets_memset(&dataConn, 0, sizeof(struct espconn));
  espconn_create(&dataConn);
  dataConn.type = ESPCONN_TCP;
  dataConn.state = ESPCONN_NONE;
  dataConn.proto.tcp = &dataConn_tcp;
  dataConn.proto.tcp->local_port = FTP_DATA_PORT_PASV;
  dataConn.proto.tcp->remote_port = FTP_DATA_PORT_PASV;
  espconn_regist_connectcb(&dataConn, dataConnConnectCb);
  espconn_accept(&dataConn);
  espconn_regist_time(&dataConn, 120, 0);
  espconn_tcp_set_max_con_allow(&dataConn, 1);
  dataStatus = NOCONNECTION;
}

static void ICACHE_FLASH_ATTR dataConnClose() {
  if (dataStatus != NODATACONN) {
    printDbg("In dataConnClose. Closing down data connection!\n");
    espconn_disconnect(&dataConn);
    espconn_delete(&dataConn);
    cmdStatus = ACCEPTINGCMD;
    dataStatus = NODATACONN;
  }
}

static void ICACHE_FLASH_ATTR cmdConnRecvCb(void *arg, char *pusrdata, unsigned short length) {
  if ((pusrdata != NULL) && (length > 0))
  {
    if (strchr(pusrdata, '\r'))
      *strchr(pusrdata, '\r') = '\0';
    processLine(pusrdata);
    printDbg("Command: '%-4s'|Parameter: '%s'\n", command, parameter);
    processCommand();
    printDbg("After process Command: '%-4s'|Parameter: '%s'\n", command, parameter);
  }
}

static void ICACHE_FLASH_ATTR cmdConnSentCb(void *arg) {
  printDbg("Previous Response was successfully transferred!\n");
}

static void ICACHE_FLASH_ATTR cmdConnDisconCb(void *arg) {
  espconn_delete((struct espconn *)arg);
}

static void ICACHE_FLASH_ATTR cmdConnConnectCb(void *arg) {
  struct espconn *pCon = arg;
  
  espconn_regist_recvcb(pCon, cmdConnRecvCb);
  espconn_regist_sentcb(pCon, cmdConnSentCb);
  espconn_regist_disconcb(pCon, cmdConnDisconCb);
  if (cmdStatus == NOTCONNECTED) {
    sendCmdConn("220 -- Welcome to FTP for ESP8266 nodemcu lua %s --", FTP_SERVER_VERSION);
    sendCmdConn("220 -- FTP server commands implemented: USER, PASS, RNTO --");
    cmdStatus = USERREQUIRED;
  }
}

static void ICACHE_FLASH_ATTR cmdConnCreate()
{
  ets_memset(&cmdConn, 0, sizeof(struct espconn));
  espconn_create(&cmdConn);
  cmdConn.type = ESPCONN_TCP;
  cmdConn.state = ESPCONN_NONE;
  cmdConn.proto.tcp = &cmdConn_tcp;
  cmdConn.proto.tcp->local_port = FTP_CTRL_PORT;
  espconn_regist_connectcb(&cmdConn, cmdConnConnectCb);
  espconn_accept(&cmdConn);
  espconn_regist_time(&cmdConn, 120, 0);
  espconn_tcp_set_max_con_allow(&cmdConn, 1);
  cmdStatus = NOTCONNECTED;
}

static void ICACHE_FLASH_ATTR cmdConnClose() {
  dataConnClose();
  if (cmdStatus != NOTCONNECTED)
  {
    sendCmdConn("%s", "221 Goodbye");
    espconn_delete(&cmdConn);
    cmdStatus = NOTCONNECTED;
  }
}

static int ICACHE_FLASH_ATTR lua_ftp_start(lua_State *L) {
  if (cmdStatus != NOTCONNECTED)
    cmdConnClose();
  cmdConnCreate();
  return (0);
}

static int ICACHE_FLASH_ATTR lua_ftp_stop(lua_State *L) {
  if (dataStatus != NODATACONN)
    dataConnClose();
  if (cmdStatus != NOTCONNECTED)
    cmdConnClose();
  return (0);
}

static const LUA_REG_TYPE ftp_func_map[] = {
  {LSTRKEY("start"), LFUNCVAL(lua_ftp_start)},
  {LSTRKEY("stop"), LFUNCVAL(lua_ftp_stop)},
  {LNILKEY, LNILVAL}};

NODEMCU_MODULE(FTP, "ftp", ftp_func_map, NULL);
