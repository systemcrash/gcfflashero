/*
 * Copyright (c) 2021-2023 dresden elektronik ingenieurtechnik gmbh.
 * All rights reserved.
 *
 * The software in this package is published under the terms of the BSD
 * style license a copy of which has been included with this distribution in
 * the LICENSE.txt file.
 *
 */

/*
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
*/

//#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <initguid.h>
#include <setupapi.h>
#include <shlwapi.h>
#include <tchar.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

#include "gcf.h"
#include "u_sstream.h"
#include "u_strlen.h"

#ifdef USE_FTD2XX
  #include "ftd2xx/ftd2xx.h"
#endif

// https://hero.handmade.network/forums/code-discussion/t/94-guide_-_how_to_avoid_c_c++_runtime_on_windows

typedef struct
{
    PL_time_t timer;
    HANDLE fd;
    uint8_t running;
    uint8_t rxbuf[64];
    uint8_t txbuf[2048];
    size_t txpos;

    LARGE_INTEGER frequency;
    BOOL frequencyValid;

    GCF *gcf;
} PL_Internal;

static PL_Internal platform;


static int GetComPort(const char *enumerator, Device *devs, size_t max);

/*! Returns a monotonic time in milliseconds. */
PL_time_t PL_Time()
{
    if (platform.frequencyValid)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return (1000LL * now.QuadPart) / platform.frequency.QuadPart;
    }

    return GetTickCount();
}

/*! Lets the programm sleep for \p ms milliseconds. */
void PL_MSleep(unsigned long ms)
{
    Sleep((DWORD)ms);
}


/*! Sets a timeout \p ms in milliseconds, after which a \c EV_TIMOUT event is generated. */
void PL_SetTimeout(unsigned long ms)
{
    //PL_Printf(DBG_DEBUG, "PL_SetTimeout(%lu ms)\n", ms);
    platform.timer = PL_Time() + ms;
}

/*! Clears an active timeout. */
void PL_ClearTimeout(void)
{
    //PL_Printf(DBG_DEBUG, "PL_ClearTimeout\n");
    platform.timer = 0;
}

/* Fills up to \p max devices in the \p devs array.

   The output is used in list operation (-l).
*/
int PL_GetDevices(Device *devs, unsigned max)
{
    // http://www.naughter.com/enumser.html

    int result = 0;

    result = GetComPort("USB", devs, max);

    if (result < (int)max)
    {
        int res2 = GetComPort("FTDIBUS", devs + result, max - result);
        if (res2 + result <= (int)max)
        {
            result += res2;
        }
    }

    return result;
}

static int GetComPort(const char *enumerator, Device *devs, size_t max)
{
    int devcount = 0;

    if (max == 0)
    {
        return devcount;
    }

    int i;
    char ch;
    U_SStream ss;
    HDEVINFO DeviceInfoSet;
    DWORD DeviceIndex =0;
    SP_DEVINFO_DATA DeviceInfoData;
    BYTE szBuffer[1024];
    DEVPROPTYPE ulPropertyType;
    DWORD dwSize = 0;
    DWORD dwType = 0;
    
    // setupDiGetClassDevs returns a handle to a device information set
    DeviceInfoSet = SetupDiGetClassDevs(
                        NULL,
                        enumerator,
                        NULL,
                        DIGCF_ALLCLASSES | DIGCF_PRESENT);
    
    if (DeviceInfoSet == INVALID_HANDLE_VALUE)
        return devcount;

    // fills a block of memory with zeros
    ZeroMemory(&DeviceInfoData, sizeof(SP_DEVINFO_DATA));
    DeviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
    // receive information about an enumerated device
    while (SetupDiEnumDeviceInfo(
                DeviceInfoSet,
                DeviceIndex,
                &DeviceInfoData) && devcount < max)
    {
        DeviceIndex++;

        if (SetupDiGetDeviceInstanceId(DeviceInfoSet, &DeviceInfoData, &szBuffer[0], sizeof(szBuffer), NULL))
        {
            U_sstream_init(&ss, &szBuffer[0], U_strlen(&szBuffer[0]));

            // USB\VID_1CF1&PID_0030\DE1995634 @ DE1995634
            // FTDIBUS\VID_0403+PID_6015+DJ00QBWEA\0000 @ 0000

            if (U_sstream_find(&ss, "VID_1CF1&PID_0030"))
            {
                strcpy_s(devs->name, sizeof(devs->name), "ConBee II");
                devs->baudrate = PL_BAUDRATE_115200;

            }
            else if (U_sstream_find(&ss, "VID_0403+PID_6015"))
            {
                strcpy_s(devs->name, sizeof(devs->name), "ConBee I");
                devs->baudrate = PL_BAUDRATE_38400;
            }
            else
            {
                continue;
            }

            if (U_sstream_find(&ss, "PID_") == 0)
                continue; /* no serial number? */

            // extract serial number
            // important: look for '+' first as the FTDI serial also contains a '\' !
            if (U_sstream_find(&ss, "+") || U_sstream_find(&ss, "\\"))
            {
                ss.pos++;
                for (i = 0; ss.pos < ss.len; ss.pos++, i++)
                {
                    if (i + 2 >= (int)sizeof(devs->serial))
                        break;

                    ch = ss.str[ss.pos];

                    if ((ch >= 'A' && ch <= 'Z') ||
                        (ch >= 'a' && ch <= 'z') ||
                        (ch >= '0' && ch <= '9'))
                    {
                        devs->serial[i] = ch;
                    }
                    else
                    {
                        break;
                    }

                }

                devs->serial[i] = '\0';
            }
            else
            {
                continue; /* no serial number? */
            }
        }
        else
        {
            continue;
        }

        // retrieves a specified Plug and Play device property
        if (SetupDiGetDeviceRegistryProperty (DeviceInfoSet, &DeviceInfoData, SPDRP_HARDWAREID,
                                              &ulPropertyType, (BYTE*)szBuffer,
                                              sizeof(szBuffer),
                                              &dwSize))
        {
            HKEY hDeviceRegistryKey;
            // get registry the key
            // HKEY_LOCAL_MACHINE\SYSTEM\ControlSet001\Control\DeviceMigration\Devices\USB

            hDeviceRegistryKey = SetupDiOpenDevRegKey(DeviceInfoSet, &DeviceInfoData,DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);

            if (hDeviceRegistryKey != INVALID_HANDLE_VALUE)
            {
                // read name of the port
                char pszPortName[20];
                dwType = 0;
                dwSize = sizeof(pszPortName);

                if ((RegQueryValueEx(hDeviceRegistryKey, "PortName", NULL, &dwType,
                    (LPBYTE) pszPortName, &dwSize) == ERROR_SUCCESS) && (dwType == REG_SZ))
                {
                    // USB\VID_1CF1&PID_0030&REV_0100 (COM3)

                    // is a com port?
                    if (_tcsnicmp(pszPortName, "COM", 3) == 0)
                    {
                        int nPortNr = atoi( pszPortName + 3);
                        if (nPortNr != 0)
                        {
                            strcpy_s(devs->path, sizeof(devs->path), pszPortName);
                            strcpy_s(devs->stablepath, sizeof(devs->path), pszPortName);
                            devcount++;
                            devs++;
                        }
                    }
                }

                RegCloseKey(hDeviceRegistryKey);
            }
        }
    }

    if (DeviceInfoSet)
    {
        SetupDiDestroyDeviceInfoList(DeviceInfoSet);
    }

    return devcount;
}

/*! Opens the serial port connection for device.

    \param path - The path like /dev/ttyACM0 or COM7.
    \returns GCF_SUCCESS or GCF_FAILED
 */
GCF_Status PL_Connect(const char *path, PL_Baudrate baudrate)
{
    char buf[32];

    if (platform.fd != INVALID_HANDLE_VALUE)
    {
        PL_Printf(DBG_DEBUG, "device already connected %s\n", path);
        return GCF_SUCCESS;
    }

    if (U_strlen(path) > 7)
    {
        return GCF_FAILED;
    }

    if (*path == 'C')
    {
        snprintf(buf, sizeof(buf), "\\\\.\\%s", path);
    }
    else if (*path == '\\')
    {
        memcpy(buf, path, strlen(path) + 1);
    }
    else
    {
        return GCF_FAILED;
    }

    platform.txpos = 0;

    platform.fd = CreateFile(
                  buf,
                  GENERIC_READ | GENERIC_WRITE,
                  0,
                  NULL,
                  OPEN_EXISTING,
                  0,
                  NULL);

    if (platform.fd == INVALID_HANDLE_VALUE)
    {
        return GCF_FAILED;
    }

    static DCB dcbSerialParams;  // Initializing DCB structure
    static COMMTIMEOUTS timeouts;  //Initializing timeouts structure

    ZeroMemory(&dcbSerialParams, sizeof(dcbSerialParams));
    ZeroMemory(&timeouts, sizeof(timeouts));

    //Setting the Parameters for the SerialPort
    BOOL Status;
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    Status = GetCommState(platform.fd, &dcbSerialParams); //retreives  the current settings
    if (Status == FALSE)
    {
        printf("\nError to Get the Com state\n\n");
        goto Exit1;
    }

    if (baudrate == PL_BAUDRATE_38400)
    {
        dcbSerialParams.BaudRate = CBR_38400;
    }
    else if (baudrate == PL_BAUDRATE_115200)
    {
        dcbSerialParams.BaudRate = CBR_115200;
    }
    else
    {
        dcbSerialParams.BaudRate = CBR_115200;
    }

    dcbSerialParams.ByteSize = 8;            //ByteSize = 8
    dcbSerialParams.StopBits = ONESTOPBIT;    //StopBits = 1
    dcbSerialParams.Parity = NOPARITY;      //Parity = None
    dcbSerialParams.fBinary = TRUE;

    Status = SetCommState(platform.fd, &dcbSerialParams);
    if (Status == FALSE)
    {
        printf("\nError to Setting DCB Structure\n\n");
        goto Exit1;
    }
    //Setting Timeouts
    timeouts.ReadIntervalTimeout = 1;
    timeouts.ReadTotalTimeoutConstant = 20;
    timeouts.ReadTotalTimeoutMultiplier = 1;
    timeouts.WriteTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    if (SetCommTimeouts(platform.fd, &timeouts) == 0)
    {
        printf("\nError to Setting Time outs");
        goto Exit1;
    }
    
    Status = SetCommMask(platform.fd, EV_RXCHAR);
    if (Status == FALSE)
    {
        printf("\nError to in Setting CommMask\n\n");
        goto Exit1;
    }

    PL_Printf(DBG_DEBUG, "connected com port %s, %lu\n", buf, (unsigned long)baudrate);

    return GCF_SUCCESS;

Exit1:
    PL_Disconnect();
    return GCF_FAILED;
}

/*! Closed the serial port connection. */
void PL_Disconnect()
{
    PL_Printf(DBG_DEBUG, "PL_Disconnect\n");
    if (platform.fd != INVALID_HANDLE_VALUE)
    {
        platform.txpos = 0;
        CloseHandle(platform.fd);
        platform.fd = INVALID_HANDLE_VALUE;
    }
    GCF_HandleEvent(platform.gcf, EV_DISCONNECTED);
}

/*! Shuts down platform layer (ends main loop). */
void PL_ShutDown()
{
    platform.running = 0;
}

/*! Executes a MCU reset for ConBee I via FTDI CBUS0 reset. */
int PL_ResetFTDI(int num, const char *serialnum)
{
    PL_Printf(DBG_INFO, "Try reset [%d] %s\n", num, serialnum);


#ifdef USE_FTD2XX

    unsigned seriallen;
    DWORD dev;
    DWORD numDevs;
    DWORD deviceId;
    FT_HANDLE ftHandle;
    FT_STATUS ftStatus;
    FT_DEVICE device;
    char serial[MAX_DEV_SERIALNR_LENGTH];
    char description[64];
    U_SStream ss;

    seriallen = U_strlen(serialnum);

    if (seriallen == 0) /* require serial number */
        return -1;

    U_sstream_init(&ss, (char*)serialnum, seriallen);

    if (FT_Initialise() != FT_OK)
    {
        return -1;
    }

    if (FT_ListDevices(&numDevs, NULL, FT_LIST_NUMBER_ONLY) != FT_OK)
    {
        return -1;
    }

    if (numDevs == 0)
    {
        return -1;
    }

    for (dev = 0; dev < numDevs; dev++)
    {
        ftStatus = FT_Open((int)dev, &ftHandle);

        if (ftStatus != FT_OK)
            continue;

        serial[0] = '\0';
        ftStatus = FT_GetDeviceInfo(ftHandle, &device, &deviceId, serial, description, NULL);

        if (ftStatus == FT_OK)
        {
            if (U_sstream_starts_with(&ss, &serial[0]))
            {
                // printf("RESET [%d] %d %s %s\n", dev, deviceId, serial, description);

                UCHAR ucMask = 0xf1; // CBUS0 --> 1

                /* ucMask - Required value for bit mode mask. This sets up which bits are inputs and
                   outputs. A bit value of 0 sets the corresponding pin to an input, a bit
                   value of 1 sets the corresponding pin to an output.
                 */
                ucMask = 0xf1; // CBUS0 --> 1
                ftStatus = FT_SetBitMode(ftHandle, ucMask, FT_BITMODE_CBUS_BITBANG);
                if (ftStatus != FT_OK)
                    goto err_close;

                ucMask = 0xf0;  // CBUS0 --> 0
                ftStatus = FT_SetBitMode(ftHandle, ucMask, FT_BITMODE_CBUS_BITBANG);
                if (ftStatus != FT_OK)
                    goto err_close;

                ucMask = 0xf1; // CBUS0 --> 1
                ftStatus = FT_SetBitMode(ftHandle, ucMask, FT_BITMODE_CBUS_BITBANG);
                if (ftStatus != FT_OK)
                    goto err_close;

                ftStatus = FT_SetBitMode(ftHandle, 0, FT_BITMODE_RESET);
                if (ftStatus != FT_OK)
                    goto err_close;

                FT_Close(ftHandle);

                return 0;
            }
        }

err_close:
        FT_Close(ftHandle);

    }
#endif /* USE_FTD2XX */

    return -1;
}

/*! Executes a MCU reset for RaspBee I / II via GPIO17 reset pin. */
int PL_ResetRaspBee()
{
    return -1;
}

int PL_ReadFile(const char *path, unsigned char *buf, unsigned long buflen)
{
    HANDLE hFile;
    int result = -1;
    DWORD nread = 0;

    hFile = CreateFile(path,
                       GENERIC_READ,
                       FILE_SHARE_READ,
                       NULL,                  // default security
                       OPEN_EXISTING,         // existing file only
                       FILE_ATTRIBUTE_NORMAL, // normal file
                       NULL);                 // no attr. template
 
    if (hFile == INVALID_HANDLE_VALUE) 
    { 
        return result; 
    }

    if (ReadFile(hFile, buf, (DWORD)buflen, &nread, NULL))
    {
        if (nread > 0)
        {
            result = (int)nread;
        }
    }

    CloseHandle(hFile);

    return result;
}


void PL_Print(const char *line)
{
    printf("%s", line);
}

void PL_Printf(DebugLevel level, const char *format, ...)
{
#ifdef NDEBUG
    if (level == DBG_DEBUG)
    {
        return;
    }
#else
    (void)level;
#endif
    va_list args;
    va_start (args, format);
    vprintf(format, args);
    va_end (args);
}

void UI_GetWinSize(unsigned *w, unsigned *h)
{
    // TODO
    *w = 80;
    *h = 60;
}

void UI_SetCursor(unsigned x, unsigned y)
{
    (void)x;
    (void)y;
}


int PROT_Write(const unsigned char *data, unsigned len)
{
    if (len == 0)
        return 0;

    Assert(platform.fd != INVALID_HANDLE_VALUE);

    BOOL Status;
    DWORD BytesWritten = 0;          // No of bytes written to the port

    //Writing data to Serial Port
    Status = WriteFile(platform.fd,// Handle to the Serialport
                       data,            // Data to be written to the port
                       len,   // No of bytes to write into the port
                       &BytesWritten,  // No of bytes written to the port
                       NULL);
    if (Status == FALSE)
    {
        DWORD dw = GetLastError();
        PL_Printf(DBG_DEBUG, "failed write com port, error: 0%08X\n", dw);
        return 0;
    }

    if (BytesWritten != (int)len)
    {
        PL_Printf(DBG_DEBUG, "failed write of %u bytes (%d written)\n", len, (int)BytesWritten);
    }
    else
    {
        gcfDebugHex(platform.gcf, "send", data, len);
    }

    return BytesWritten;
}

int PROT_Putc(unsigned char ch)
{
    Assert(platform.txpos + 1 < sizeof(platform.txbuf));
    if (platform.txpos + 1 < sizeof(platform.txbuf))
    {
        platform.txbuf[platform.txpos] = ch;
        platform.txpos += 1;
        return 1;
    }
    return 0;
}

int PROT_Flush()
{
    int result = 0;

    if (platform.txpos != 0 && platform.txpos < sizeof(platform.txbuf))
    {
        result = PROT_Write(&platform.txbuf[0], (unsigned)platform.txpos);
        Assert(result == (int)platform.txpos); /* support/handle partial writes? */
        platform.txpos = 0;
    }

    return result;
}

static void PL_Loop(GCF *gcf)
{
    ZeroMemory(&platform, sizeof(platform));
    platform.gcf = gcf;
    platform.fd = INVALID_HANDLE_VALUE;

    platform.running = 1;
    platform.frequencyValid = QueryPerformanceFrequency(&platform.frequency);

    GCF_HandleEvent(gcf, EV_PL_STARTED);

    BOOL Status;
    while (platform.running)
    {
        if (platform.fd == INVALID_HANDLE_VALUE)
        {
            Sleep(20);

            if (platform.timer != 0)
            {
                if (platform.timer < PL_Time())
                {
                    platform.timer = 0;
                    GCF_HandleEvent(gcf, EV_TIMEOUT);
                }
            }

            continue;
        }

        DWORD NoBytesRead;
        Status = ReadFile(platform.fd, &platform.rxbuf, sizeof(platform.rxbuf), &NoBytesRead, NULL);

        if (Status == FALSE)
        {
            PL_Disconnect();
            continue;
        }
        else if (NoBytesRead > 0)
        {
            GCF_Received(gcf, platform.rxbuf, NoBytesRead);
        }
        else if (NoBytesRead == 0)
        {
            if (platform.timer != 0)
            {
                if (platform.timer < PL_Time())
                {
                    platform.timer = 0;
                    GCF_HandleEvent(gcf, EV_TIMEOUT);
                }
            }
            else
            {
                Sleep(4);
            }
        }
    }
}

int main(int argc, char **argv)
{
    GCF *gcf = GCF_Init(argc, argv);
    if (gcf == NULL)
        return 2;

    PL_Loop(gcf);

    GCF_Exit(gcf);

    return 0;
}
