//////////////////////////////////////////////////////////////////////////////
//                                                                          //
//                                     _                      _             //
//        __  __ _ __ ___    ___    __| |  ___  _ __ ___     | |__          //
//        \ \/ /| '_ ` _ \  / _ \  / _` | / _ \| '_ ` _ \    | '_ \         //
//         >  < | | | | | || (_) || (_| ||  __/| | | | | | _ | | | |        //
//        /_/\_\|_| |_| |_| \___/  \__,_| \___||_| |_| |_|(_)|_| |_|        //
//                                                                          //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////
//                                                                          //
//          Copyright (c) 2012 by S.F.T. Inc. - All rights reserved         //
//  Use, copying, and distribution of this software are licensed according  //
//    to the LGPLv2.1, or a BSD-like license, as appropriate (see below)    //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////

/** \mainpage S.F.T. XMODEM library
  *
  * Copyright (c) 2012 by S.F.T. Inc. - All rights reserved\n
  *
  * The source files include DOXYGEN SUPPORT to properly document the library
  * Please excuse the additional comments necessary to make this work.
  * Instead, build the doxygen output and view the documentation, as
  * well as the code itself WITHOUT all of the doxygen markup comments.
  * \n
  * \n
  * This library was designed to work with POSIX-compliant operating systems
  * such as Linux, FreeBSD, and OSX, and also on Arduino microcontrollers.
  * The intent was to provide an identical code base for both ends of the
  * XMODEM transfer, compilable as either C or C++ code for maximum flexibility.
  *
  * Normally you will only need to use one of these two functions:\n
  * \n
  * \ref XSend() - send a file via XMODEM\n
  * \ref XReceive() - receive a file via XMODEM\n
  * \n
  * The rest of the documentation was provided to help you debug any problems,
  * or even to write your own library (as appropriate).\n
  *
  * LICENSE
  *
  * This software is licensed under either the LGPLv2 or a BSD-like license.
  * For more information, see\n
  *   http://opensource.org/licenses/BSD-2-Clause\n
  *   http://www.gnu.org/licenses/lgpl-2.1.html\n
  * and the above copyright notice.\n
  * \n
  * In short, you may use this software anyway you like, provided that you
  * do not hold S.F.T. Inc. responsible for consequential or inconsequential
  * damages resulting from use, modification, abuse, or anything else done
  * with this software, and you include the appropriate license (either LGPLv2
  * or a BSD-like license) and comply with the requirements of said license.\n
  * So, if you use a BSD-like license, you can copy the license template at
  * the abovementioned URL and sub in the copyright notice as shown above.
  * Or, you may use an LGPLv2 license, and then provide source files with a
  * re-distributed or derived work (including a complete re-write with this
  * library as a template).  A link back to the original source, of course,
  * would be appreciated but is not required.
**/

/** \file xmodem.h
  * \brief main header file for S.F.T. XMODEM library
  *
  * S.F.T. XMODEM library
**/

/** \defgroup xmodem_api XModem API
  * high-level API functions
*/

/** \defgroup xmodem_internal XModem Internal
  * internal support functions
*/



#include <stdlib.h>

// required include files

#if WIN32
// win32 includes
#include <Windows.h>
#include <io.h>
#else // POSIX
// posix includes
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/ioctl.h> // for IOCTL definitions
#include <memory.h>
#endif // OS-dependent includes

#if defined(WIN32) // WINDOWS

// file and serial types for WIN32
#define FILE_TYPE HANDLE
#define SERIAL_TYPE HANDLE

#else // POSIX

// file and serial types for POSIX
#define FILE_TYPE int
#define SERIAL_TYPE int

#endif


// common definitions

#define SILENCE_TIMEOUT 5000 /* 5 seconds */
#define TOTAL_ERROR_COUNT 32
#define ACK_ERROR_COUNT 8


// Arduino build uses C++ so I must define functions properly


#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

/** \ingroup xmodem_api
  * \brief Receive a file using XMODEM protocol
  *
  * \param hSer A 'HANDLE' for the open serial connection
  * \param szFilename A pointer to a (const) 0-byte terminated string containing the file name
  * \param nMode The file mode to be used on create (RWX bits)
  * \return A value of zero on success, negative on failure, positive if canceled
  *
  * Call this function to receive a file, passing the handle to the open serial connection, and the
  * name and mode of the file to create from the XMODEM stream.  The function will return a value of zero on
  * success.  On failure or cancelation, the file will be deleted.\n
  * If the specified file exists before calling this function, it will be overwritten.  If you do not
  * want to unconditionally overwrite an existing file, you should test to see if it exists first.
  *
**/
int XReceive(SERIAL_TYPE hSer, const char *szFilename, int nMode);

/** \ingroup xmodem_api
  * \brief Send a file using XMODEM protocol
  *
  * \param hSer A 'HANDLE' for the open serial connection
  * \param szFilename A pointer to a (const) 0-byte terminated string containing the file name
  * \return A value of zero on success, negative on failure, positive if canceled
  *
  * Call this function to receive a file, passing the handle to the open serial connection, and the
  * name and mode of the file to send via the XMODEM stream.  The function will return a value of zero on
  * success.  If the file does not exist, the function will return a 'failure' value and cancel
  * the transfer.
  *
**/
int XSend(SERIAL_TYPE hSer, const char *szFilename);


#ifdef __cplusplus
};
#endif // __cplusplus

