//////////////////////////////////////////////////////////////////////////////
//                                                                          //
//                                      _                                   //
//         __  __ _ __ ___    ___    __| |  ___  _ __ ___      ___          //
//         \ \/ /| '_ ` _ \  / _ \  / _` | / _ \| '_ ` _ \    / __|         //
//          >  < | | | | | || (_) || (_| ||  __/| | | | | | _| (__          //
//         /_/\_\|_| |_| |_| \___/  \__,_| \___||_| |_| |_|(_)\___|         //
//                                                                          //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////
//                                                                          //
//          Copyright (c) 2012 by S.F.T. Inc. - All rights reserved         //
//  Use, copying, and distribution of this software are licensed according  //
//    to the GPLv2, LGPLv2, or BSD license, as appropriate (see COPYING)    //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////


// XMODEM adapted for arduino and POSIX systems.  Windows code incomplete

#include "xmodem.h"

#include <stdint.h>

/* Kernel includes. */
#include "FreeRTOS.h"

// internal structure definitions

// Windows requires a different way of specifying structure packing
#ifdef WIN32
#define PACKED
#pragma pack(push,1)
#else // POSIX, ARDUINO
#define PACKED __attribute__((__packed__))
#endif // WIN32 vs THE REST OF THE WORLD

#define _SOH_ 1 /* start of packet - note XMODEM-1K uses '2' */
#define _EOT_ 4
#define _ENQ_ 5
#define _ACK_ 6
#define _NAK_ 21 /* NAK character */
#define _CAN_ 24 /* CAN character CTRL+X */

/** \file xmodem.c
  * \brief main source file for S.F.T. XMODEM library
  *
  * S.F.T. XMODEM library
**/

/** \ingroup xmodem_internal
  * \brief Structure defining an XMODEM CHECKSUM packet
  *
\code
typedef struct _XMODEM_BUF_
{
   char cSOH;                   // ** SOH byte goes here             **
   unsigned char aSEQ, aNotSEQ; // ** 1st byte = seq#, 2nd is ~seq#  **
   char aDataBuf[128];          // ** the actual data itself!        **
   unsigned char bCheckSum;     // ** checksum gets 1 byte           **
} PACKED XMODEM_BUF;

\endcode
  *
**/
typedef struct _XMODEM_BUF_
{
   char cSOH;                   ///< SOH byte goes here
   unsigned char aSEQ, aNotSEQ; ///< 1st byte = seq#, 2nd is ~seq#
   char aDataBuf[128];          ///< the actual data itself!
   unsigned char bCheckSum;     ///< checksum gets 1 byte
} PACKED XMODEM_BUF;

#ifdef WIN32
// restore default packing
#pragma pack(pop)
#endif // WIN32

/** \ingroup xmodem_internal
  * \brief Structure that identifies the XMODEM communication state
  *
\code
typedef struct _XMODEM_
{
  SERIAL_TYPE ser;     // identifies the serial connection, data type is OS-dependent
  FILE_TYPE file;      // identifies the file handle, data type is OS-dependent

  XMODEM_BUF xbuf;   // XMODEM CHECKSUM buffer

} XMODEM;

\endcode
  *
**/
typedef struct _XMODEM_
{
  SERIAL_TYPE ser;     ///< identifies the serial connection, data type is OS-dependent
  FILE_TYPE file;      ///< identifies the file handle, data type is OS-dependent

  XMODEM_BUF xbuf;   ///< XMODEM CHECKSUM buffer
} XMODEM;


/** \ingroup xmodem_internal
  * \brief Calculate checksum for XMODEM packet
  *
  * \param lpBuf A pointer to the XMODEM data buffer
  * \param cbBuf The length of the XMODEM data buffer (typically 128)
  * \return An unsigned char value to be assigned to the 'checksum' element in the XMODEM packet
  *
**/
unsigned char CalcCheckSum(const char *lpBuf, short cbBuf)
{
short iC, i1;

  iC = 0;

  for(i1 = 0; i1 < cbBuf; i1++)
  {
    iC += lpBuf[i1];
  }

  return (unsigned char)(iC & 0xff);
}

//#define MyMillis() GetTickCount()
#define MyMillis() ((uint32_t)xTaskGetTickCount() * 1000 / configTICK_RATE_HZ)

/** \ingroup xmodem_internal
  * \brief Generate a sequence number pair, place into XMODEM_BUF
  *
  * \param pBuf A pointer to an XMODEM_BUF structure
  * \param bSeq An unsigned char, typically cast from an unsigned long 'block number'
  *
  * This function generates the sequence pair for the XMODEM packet.  The 'block number'
  * is initially assigned a value of '1', and increases by 1 for each successful packet.
  * That value is 'truncated' to a single byte and assigned as a sequence number for the
  * packet itself.
**/
void GenerateSEQ(XMODEM_BUF *pBuf, unsigned char bSeq)
{
  pBuf->aSEQ = bSeq;
  pBuf->aNotSEQ = ~bSeq;
}

/** \ingroup xmodem_internal
  * \brief Get an XMODEM block from the serial device
  *
  * \param ser A 'SERIAL_TYPE' identifier for the serial connection
  * \param pBuf A pointer to the buffer that receives the data
  * \param cbSize The number of bytes/chars to read
  * \return The number of bytes/chars read, 0 if timed out (no data), < 0 on error
  *
  * Call this function to read data from the serial port, specifying the number of
  * bytes to read.  This function times out after no data transferred (silence) for
  * a period of 'SILENCE_TIMEOUT' milliseconds.  This allows spurious data transfers
  * to continue as long as there is LESS THAN 'SILENCE_TIMEOUT' between bytes, and
  * also allows VERY SLOW BAUD RATES (as needed).  However, if the transfer takes longer
  * than '10 times SILENCE_TIMEOUT', the function will return the total number of bytes
  * that were received within that time.\n
  * The default value of 5 seconds, extended to 50 seconds, allows a worst-case baud
  * rate of about 20.  This should not pose a problem.  If it does, edit the code.
**/
short GetXmodemBlock(SERIAL_TYPE ser, char *pBuf, short cbSize)
{
unsigned long ulCur;
short cb1;
char *p1;

// ** This function obtains a buffer of 'wSize%' bytes,      **
// ** waiting a maximum of 5 seconds (of silence) to get it. **
// ** It returns this block as a string of 'wSize%' bytes,   **
// ** or a zero length string on error.                      **

//   iDisableRXOVER% = 1; // bug workaround

#ifdef WIN32
// TODO:

#else
// POSIX
int i1, i2;
unsigned long ulStart;


  if(fcntl(ser, F_SETFL, O_NONBLOCK) == -1)
  {
    static int iFailFlag = 0;

    if(!iFailFlag)
    {
      fprintf(stderr, "Warning:  'fcntl(O_NONBLOCK)' failed, errno = %d\n", errno);
      fflush(stderr);
      iFailFlag = 1;
    }
  }

  p1 = pBuf;
  cb1 = 0;

  ulStart = ulCur = MyMillis();

  for(i1=0; i1 < cbSize; i1++)
  {
    while((i2 = read(ser, p1, 1)) != 1)
    {
      if(i2 < 0 && errno != EAGAIN)
      {
        // read error - exit now
//        return cb1; // how many bytes I actually read
        goto the_end;
      }
      else
      {
        usleep(1000); // 1 msec

        if((MyMillis() - ulCur) > SILENCE_TIMEOUT || // too much silence?
           (MyMillis() - ulStart) > 10 * SILENCE_TIMEOUT) // too long for transfer
        {
//          return cb1; // finished (return how many bytes I actually read)
          goto the_end;
        }
      }
    }

    // here it succeeds

    cb1++;
    p1++;

    if((MyMillis() - ulStart) > 10 * SILENCE_TIMEOUT) // 10 times SILENCE TIMEOUT for TOTAL TIMEOUT
    {
      break; // took too long, I'm going now
    }
  }

the_end:
#endif
  return cb1; // what I actually read
}

/** \ingroup xmodem_internal
  * \brief Write a single character to the serial device
  *
  * \param ser A 'SERIAL_TYPE' identifier for the serial connection
  * \param bVal The byte to send
  * \return The number of bytes/chars written, or < 0 on error
  *
  * Call this function to write one byte of data to the serial port.  Typically
  * this is used to send things like an ACK or NAK byte.
**/
int WriteXmodemChar(SERIAL_TYPE ser, unsigned char bVal)
{
int iRval;

#if defined(WIN32)

// TODO:

#else // POSIX
char buf[2]; // use size of '2' to avoid warnings about array size of '1'

  if(fcntl(ser, F_SETFL, 0) == -1) // set blocking mode
  {
    static int iFailFlag = 0;

    if(!iFailFlag)
    {
      fprintf(stderr, "Warning:  'fcntl(O_NONBLOCK)' failed, errno = %d\n", errno);
      iFailFlag = 1;
    }
  }

  buf[0] = bVal; // in case args are passed by register

  iRval = write(ser, buf, 1);
#endif

  return iRval;
}

/** \ingroup xmodem_internal
  * \brief Send an XMODEM block via the serial device
  *
  * \param ser A 'SERIAL_TYPE' identifier for the serial connection
  * \param pBuf A pointer to the buffer that receives the data
  * \param cbSize The number of bytes/chars to write
  * \return The number of bytes/chars written, < 0 on error
  *
  * Call this function to write data via the serial port, specifying the number of
  * bytes to write.
**/
int WriteXmodemBlock(SERIAL_TYPE ser, const void *pBuf, int cbSize)
{
int iRval;

#if defined(WIN32)

// TODO:

#else // POSIX

  if(fcntl(ser, F_SETFL, 0) == -1) // set blocking mode
  {
    static int iFailFlag = 0;

    if(!iFailFlag)
    {
      fprintf(stderr, "Warning:  'fcntl(O_NONBLOCK)' failed, errno = %d\n", errno);
      fflush(stderr);
      iFailFlag = 1;
    }
  }

  iRval = write(ser, pBuf, cbSize);

#endif

  return iRval;
}

/** \ingroup xmodem_internal
  * \brief Read all input from the serial port until there is 1 second of 'silence'
  *
  * \param ser A 'SERIAL_TYPE' identifier for the serial connection
  *
  * Call this function to read ALL data from the serial port, until there is a period
  * with no data (i.e. 'silence') for 1 second.  At that point the function will return.\n
  * Some operations require that any bad data be flushed out of the input to prevent
  * synchronization problems.  By using '1 second of silence' it forces re-synchronization
  * to occur in one shot, with the possible exception of VERY noisy lines.  The down side
  * is that it may slow down transfers with a high data rate.
**/
void XModemFlushInput(SERIAL_TYPE ser)
{
unsigned long ulStart;

#if defined(WIN32)

// TODO:

#else // POSIX
int i1;

unsigned char buf[2];

  if(fcntl(ser, F_SETFL, O_NONBLOCK) == -1)
  {
    static int iFailFlag = 0;

    if(!iFailFlag)
    {
      fprintf(stderr, "Warning:  'fcntl(O_NONBLOCK)' failed, errno = %d\n", errno);
      iFailFlag = 1;
    }
  }

  ulStart = MyMillis();

  while((MyMillis() - ulStart) < 1000)
  {

    i1 = read(ser, buf, 1);
    if(i1 == 1)
    {
      ulStart = MyMillis();
    }
    else
    {
      usleep(1000);
    }
  }

#endif
}

/** \ingroup xmodem_internal
  * \brief Terminate the XMODEM connection
  *
  * \param pX A pointer to the 'XMODEM' object identifying the transfer
  *
  * Call this function prior to ending the XMODEM transfer.  Currently the only
  * thing it does is flush the input.
**/
void XmodemTerminate(XMODEM *pX)
{
  XModemFlushInput(pX->ser);

  // TODO:  close files?
}


/** \ingroup xmodem_internal
  * \brief Validate the sequence number of a received XMODEM block
  *
  * \param pX A pointer to an 'XMODEM_BUF'
  * \param bSeq The expected sequence number (block & 255)
  * \return A zero value on success, non-zero otherwise
  *
  * Call this function to validate a packet's sequence number against the block number
**/
short ValidateSEQ(XMODEM_BUF *pX, unsigned char bSeq)
{
  return pX->aSEQ != 255 - pX->aNotSEQ || // ~(pX->aNotSEQ) ||
         pX->aSEQ != bSeq; // returns TRUE if not valid
}

/** \ingroup xmodem_internal
  * \brief Generic function to receive a file via XMODEM (CRC or Checksum)
  *
  * \param pX A pointer to an 'XMODEM_BUF' with valid bCRC, ser, and file members
  * \return A zero value on success, negative on error, positive on cancel
  *
  * The calling function will need to poll for an SOH from the server using 'C' and 'NAK'
  * characters (as appropriate) until an SOH is received.  That value must be assigned
  * to the 'buf' union (as appropriate), and the bCRC member assigned to non-zero if
  * the server responded to 'C', or zero if it responded to 'NAK'.  With the bCRC,
  * ser, and file members correctly assigned, call THIS function to receive content
  * via XMODEM and write it to 'file'.\n
  * This function will return zero on success, a negative value on error, and a positive
  * value if the transfer was canceled by the server.
**/
int ReceiveXmodem(XMODEM *pX)
{
int ecount, ec2;
long etotal, filesize, block;
unsigned char cY; // the char to send in response to a packet

  ecount = 0;
  etotal = 0;
  filesize = 0;
  block = 1;

  // ** already got the first 'SOH' character on entry to this function **

  pX->xbuf.cSOH = (char)1; // assumed already got this, put into buffer

  do
  {
    if( (( GetXmodemBlock(pX->ser, ((char *)&(pX->xbuf)) + 1, sizeof(pX->xbuf) - 1))
        != sizeof(pX->xbuf) - 1 ||
        ( ValidateSEQ(&(pX->xbuf), block & 255)) ||
        ( CalcCheckSum(pX->xbuf.aDataBuf, sizeof(pX->xbuf.aDataBuf)) != pX->xbuf.bCheckSum)))
    {
      // did not receive properly
      // TODO:  deal with repeated packet, sequence number for previous packet

      XModemFlushInput(pX->ser);  // necessary to avoid problems

      cY = _NAK_; // send NAK (to get the checksum version)
      ecount ++; // for this packet
      etotal ++;
    }
    else
    {
      if(write(pX->file, &(pX->xbuf.aDataBuf), sizeof(pX->xbuf.aDataBuf)) != sizeof(pX->xbuf.aDataBuf))
      {
        XmodemTerminate(pX);
        return -2; // write error on output file
      }
      cY = _ACK_; // send ACK
      block ++;
      filesize += sizeof(pX->xbuf.aDataBuf); // TODO:  need method to avoid extra crap at end of file
      ecount = 0; // zero out error count for next packet
    }

    ec2 = 0;   //  ** error count #2 **

    while(ecount < TOTAL_ERROR_COUNT && ec2 < ACK_ERROR_COUNT) // ** loop to get SOH or EOT character **
    {
      WriteXmodemChar(pX->ser, cY); // ** output appropriate command char **
     
      if(GetXmodemBlock(pX->ser, &(pX->xbuf.cSOH), 1) == 1)
      {
        if(pX->xbuf.cSOH == _CAN_) // ** CTRL-X 'CAN' - terminate
        {
          XmodemTerminate(pX);
          return 1; // terminated
        }
        else if(pX->xbuf.cSOH == _EOT_) // ** EOT - end
        {
          WriteXmodemChar(pX->ser, _ACK_); // ** send an ACK (most XMODEM protocols expect THIS)

          return 0; // I am done
        }
        else if(pX->xbuf.cSOH == _SOH_) // ** SOH - sending next packet
        {
          break; // leave this loop
        }
        else
        {
          // TODO:  deal with repeated packet, i.e. previous sequence number

          XModemFlushInput(pX->ser);  // necessary to avoid problems (since the character was unexpected)
          // if I was asking for the next block, and got an unexpected character, do a NAK; otherwise,
          // just repeat what I did last time

          if(cY == _ACK_) // ACK
          {
            cY = _NAK_; // NACK
          }

          ec2++;
        }
      }
      else
      {
        ecount++; // increase total error count, and try writing the 'ACK' or 'NACK' again
      }
    }

    if(ec2 >= ACK_ERROR_COUNT) // wasn't able to get a packet
    {
      break;
    }

  } while(ecount < TOTAL_ERROR_COUNT);

  XmodemTerminate(pX);
  return 1; // terminated
}


/** \ingroup xmodem_internal
  * \brief Generic function to send a file via XMODEM (Checksum)
  *
  * \param pX A pointer to an 'XMODEM_BUF' with valid ser, and file members, and the polled
  * 'NAK' value assigned to the cSOH member (first byte) within the 'xbuf'.
  * \return A zero value on success, negative on error, positive on cancel
  *
  * The calling function will need to poll for a 'C' or NAK from the client (as appropriate)
  * and assign that character to the cSOH member in the xbuf since
  * the 'cSOH' will always be the first byte).  Then call this function to send content
  * via XMODEM from 'file'.\n
  * It is important to record the NAK character before calling this function since the 'C' or
  * 'NAK' value will be used to determine whether to use CRC or CHECKSUM.\n
  * This function will return zero on success, a negative value on error, and a positive
  * value if the transfer was canceled by the receiver.
**/
int SendXmodem(XMODEM *pX)
{
int ecount, ec2;
short i1;
long etotal, filesize, filepos, block;


  ecount = 0;
  etotal = 0;
  filesize = 0;
  filepos = 0;
  block = 1;

  // ** already got first 'NAK' character on entry as pX->xbuf.cSOH  **

  filesize = (long)lseek(pX->file, 0, SEEK_END);
  if(filesize < 0) // not allowed
  {
    return -1;
  }

  lseek(pX->file, 0, SEEK_SET); // position at beginning

  do
  {
    // ** depending on type of transfer, place the packet
    // ** into pX->xbuf with all fields appropriately filled.

    if(filepos >= filesize) // end of transfer
    {
      for(i1=0; i1 < 8; i1++)
      {
        WriteXmodemChar(pX->ser, _EOT_); // ** send an EOT marking end of transfer

        if(GetXmodemBlock(pX->ser, &(pX->xbuf.cSOH), 1) != 1) // this takes up to 5 seconds
        {
          // nothing returned - try again?
          // break; // for now I loop, uncomment to bail out
        }
        else if(pX->xbuf.cSOH == _ENQ_    // an 'ENQ' (apparently some expect this)
                || pX->xbuf.cSOH == _ACK_ // an 'ACK' (most XMODEM implementations expect this)
                || pX->xbuf.cSOH == _CAN_) // CTRL-X = TERMINATE
        {
          // both normal and 'abnormal' termination.
          break;
        }
      }

      XmodemTerminate(pX);

      return i1 >= 8 ? 1 : 0; // return 1 if receiver choked on the 'EOT' marker, else 0 for 'success'
    }


    if(pX->xbuf.cSOH != 'C' // XMODEM CRC
       && pX->xbuf.cSOH != (char)_NAK_) // NAK
    {
      // increase error count, bail if it's too much

      ec2++;
    }

    lseek(pX->file, filepos, SEEK_SET); // same reason as above

    // fortunately, xbuf and xcbuf are the same through the end of 'aDataBuf' so
    // I can read the file NOW using 'xbuf' for both CRC and CHECKSUM versions

    if((filesize - filepos) >= sizeof(pX->xbuf.aDataBuf))
    {
      i1 = read(pX->file, pX->xbuf.aDataBuf, sizeof(pX->xbuf.aDataBuf));

      if(i1 != sizeof(pX->xbuf.aDataBuf))
      {
        // TODO:  read error - send a ctrl+x ?
      }
    }
    else
    {
      memset(pX->xbuf.aDataBuf, '\x1a', sizeof(pX->xbuf.aDataBuf)); // fill with ctrl+z which is what the spec says

      i1 = read(pX->file, pX->xbuf.aDataBuf, filesize - filepos);

      if(i1 != (filesize - filepos))
      {
        // TODO:  read error - send a ctrl+x ?
      }
    }

    if(pX->xbuf.cSOH == _NAK_ || // 'NAK' (checksum method, may also be with CRC method)
            (pX->xbuf.cSOH == _ACK_)) // identifies ACK with XMODEM CHECKSUM
    {
      // calculate the CHECKSUM, assign to the packet, and then send it

      pX->xbuf.cSOH = 1; // must send SOH as 1st char
      pX->xbuf.bCheckSum = CalcCheckSum(pX->xbuf.aDataBuf, sizeof(pX->xbuf.aDataBuf));

      GenerateSEQ(&(pX->xbuf), block);

      // send it

      i1 = WriteXmodemBlock(pX->ser, &(pX->xbuf), sizeof(pX->xbuf));
      if(i1 != sizeof(pX->xbuf)) // write error
      {
        // TODO:  handle write error (send ctrl+X ?)
      }
    }

    ec2 = 0;

    while(ecount < TOTAL_ERROR_COUNT && ec2 < ACK_ERROR_COUNT) // loop to get ACK or NACK
    {
      if(GetXmodemBlock(pX->ser, &(pX->xbuf.cSOH), 1) == 1)
      {
        if(pX->xbuf.cSOH == _CAN_) // ** CTRL-X - terminate
        {
          XmodemTerminate(pX);

          return 1; // terminated
        }
        else if(pX->xbuf.cSOH == _NAK_ || // ** NACK
                pX->xbuf.cSOH == 'C') // ** CRC NACK
        {
          break;  // exit inner loop and re-send packet
        }
        else if(pX->xbuf.cSOH == _ACK_) // ** ACK - sending next packet
        {
          filepos += sizeof(pX->xbuf.aDataBuf);
          block++; // increment file position and block count

          break; // leave inner loop, send NEXT packet
        }
        else
        {
          XModemFlushInput(pX->ser);  // for now, do this here too
          ec2++;
        }
      }
      else
      {
        ecount++; // increase total error count, then loop back and re-send packet
        break;
      }
    }

    if(ec2 >= ACK_ERROR_COUNT)
    {
      break;  // that's it, I'm done with this
    }

  } while(ecount < TOTAL_ERROR_COUNT); // twice error count allowed for sending

   // ** at this point it is important to indicate the errors
   // ** and flush all buffers, and terminate process!

  XmodemTerminate(pX);

  return -2; // exit on error
}


/** \ingroup xmodem_internal
  * \brief Calling function for ReceiveXmodem
  *
  * \param pX A pointer to an 'XMODEM_BUF' with valid ser, and file members
  * \return A zero value on success, negative on error, positive on cancel
  *
  * This is a generic 'calling function' for ReceiveXmodem that checks for
  * a response to 'C' and 'NAK' characters, and sets up the XMODEM transfer
  * for either CRC or CHECKSUM mode.\n
  * This function will return zero on success, a negative value on error, and a positive
  * value if the transfer was canceled by the receiver.
**/
int XReceiveSub(XMODEM *pX)
{
int i1;

  // try again, this time using XMODEM CHECKSUM
  for(i1=0; i1 < 8; i1++)
  {
    WriteXmodemChar(pX->ser, _NAK_); // switch to NAK for XMODEM Checksum
     
    if(GetXmodemBlock(pX->ser, &(pX->xbuf.cSOH), 1) == 1)
    {
      if(pX->xbuf.cSOH == _SOH_) // SOH - packet is on its way
      {
        return ReceiveXmodem(pX);
      }
      else if(pX->xbuf.cSOH == _EOT_) // an EOT [blank file?  allow this?]
      {
        return 0; // for now, do this
      }
      else if(pX->xbuf.cSOH == _CAN_) // cancel
      {
        return 1; // canceled
      }
    }
  }    
  

  XmodemTerminate(pX);

  return -3; // fail
}


/** \ingroup xmodem_internal
  * \brief Calling function for SendXmodem
  *
  * \param pX A pointer to an 'XMODEM_BUF' with valid ser, and file members
  * \return A zero value on success, negative on error, positive on cancel
  *
  * This is a generic 'calling function' for SendXmodem that checks for polls by the
  * receiver, and places the 'NAK' or 'C' character into the 'buf' member of the XMODEM
  * structure so that SendXmodem can use the correct method, either CRC or CHECKSUM mode.\n
  * This function will return zero on success, a negative value on error, and a positive
  * value if the transfer was canceled by the receiver.
**/
int XSendSub(XMODEM *pX)
{
unsigned long ulStart;

  // waiting up to 30 seconds for transfer to start.  this is part of the spec?

  ulStart = MyMillis();

  do
  {
    if(GetXmodemBlock(pX->ser, &(pX->xbuf.cSOH), 1) == 1)
    {
      if(pX->xbuf.cSOH == 'C' || // XMODEM CRC
         pX->xbuf.cSOH == _NAK_) // NAK - XMODEM CHECKSUM
      {
        return SendXmodem(pX);
      }
      else if(pX->xbuf.cSOH == _CAN_) // cancel
      {
        return 1; // canceled
      }
    }
  }    
  while((int)(MyMillis() - ulStart) < 30000);

  XmodemTerminate(pX);

  return -3; // fail
}

int XReceive(SERIAL_TYPE hSer, const char *szFilename)
{
int iRval;
XMODEM xx;

#ifdef WIN32
// TODO:
#else
// POSIX

int iFlags;

  memset(&xx, 0, sizeof(xx));

  xx.ser = hSer;

  unlink(szFilename); // make sure it does not exist, first
  xx.file = open(szFilename, O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);

  if(!xx.file)
  {
    return -9; // can't create file
  }

  iFlags = fcntl(hSer, F_GETFL);

  iRval = XReceiveSub(&xx);  

  if(iFlags == -1 || fcntl(hSer, F_SETFL, iFlags) == -1)
  {
    fprintf(stderr, "Warning:  'fcntl' call to restore flags failed, errno=%d\n", errno);
  }

  close(xx.file);

  if(iRval)
  {
    unlink(szFilename); // delete file on error
  }
#endif
  return iRval;
}

int XSend(SERIAL_TYPE hSer, const char *szFilename)
{
int iRval;
XMODEM xx;

#ifdef WIN32

// TODO:

#else
// POSIX
int iFlags;

  memset(&xx, 0, sizeof(xx));

  xx.ser = hSer;

  xx.file = open(szFilename, O_RDONLY, 0);

  if(!xx.file)
  {
    return -9; // can't open file
  }

  iFlags = fcntl(hSer, F_GETFL);

  iRval = XSendSub(&xx);  

  if(iFlags == -1 || fcntl(hSer, F_SETFL, iFlags) == -1)
  {
    fprintf(stderr, "Warning:  'fcntl' call to restore flags failed, errno=%d\n", errno);
  }

  close(xx.file);
#endif
  return iRval;
}

