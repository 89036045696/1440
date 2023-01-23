
/* Standard includes. */
#include <stdint.h>
#include <stdio.h>

/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "task.h"

/* FreeRTOS+TCP includes. */
#include "FreeRTOS_Sockets.h"

// для макросов порядка байтов
#include "FreeRTOS_IP.h"



#include "ConnectionTasks.h"

#ifdef WIN32
#define PACKED #pragma pack(push,1)
#else // POSIX, ARDUINO
#define PACKED __attribute__((__packed__))
#endif

пробное изменение1
/*-----------------------------------------------------------*/

/*******************************************************************************
* Возможные значения поля "команда" принимаемого пакета с командой.
*******************************************************************************/

const uint8_t CMD_READ = 0;
const uint8_t CMD_WRITE = 1;
/*-----------------------------------------------------------*/
#ifdef WIN32
#pragma pack(push,1)
#endif
// описывает форматы входящих пакетов
union TagRxBuffer
{
    uint8_t bytes[10];

    struct TagRxBufFormatCmdRead
    {
        uint8_t Id;
        uint8_t Cmd;
        uint32_t RegAddr;       // network endian
    } FormatCmdRead;

    struct TagRxBufFormatCmdWrite
    {
        uint8_t Id;
        uint8_t Cmd;
        uint32_t RegAddr;       // network endian
        uint32_t ValueToWrite;  // network endian
    } FormatCmdWrite;

    struct TagRxBufFormatAckReply
    {
        uint8_t Id;
        uint8_t Ack;
    } FormatAckReply;
};
#ifdef WIN32
// restore default packing
#pragma pack(pop)
#endif
//------------------------------------------------------------------------------
#ifdef WIN32
#pragma pack(push,1)
#endif
// описывает форматы исходящих пакетов
union TagTxBuffer
{
    uint8_t bytes[5];

    struct TagTxBufFormatCmd
    {
        uint8_t Id;
        uint32_t RDataOrWStatus;      // network endian
    } FormatCmd;

    struct TagTxBufFormatAckRequest
    {
        uint8_t Id;
        uint8_t Ack;
    } FormatAckRequest;
};
#ifdef WIN32
// restore default packing
#pragma pack(pop)
#endif




void task1UDPConnection( void *pvParameters )
{
    // RX blocking time = infinity
    const TickType_t rxTimeOut = portMAX_DELAY;
    FreeRTOS_setsockopt(((struct TagParamsOfUDPConnectionTask*)pvParameters)->ClientSocket,
        0,
        FREERTOS_SO_RCVTIMEO,
        &rxTimeOut,
        sizeof(rxTimeOut));

    union TagRxBuffer rxBuf;
    union TagTxBuffer txBuf;
    while (1)
    {
        /* Read new RX packet with command */
        int32_t rxNum = FreeRTOS_recvfrom(((struct TagParamsOfUDPConnectionTask*)pvParameters)->ClientSocket,
            &rxBuf, sizeof(rxBuf),
            0 /* ulFlags with the FREERTOS_ZERO_COPY bit clear. */,
            &((struct TagParamsOfUDPConnectionTask*)pvParameters)->DestinationAddress,
            sizeof(struct freertos_sockaddr) /* Not used but should be set as shown. */
        );
        if (rxNum <= 0) { continue; } // continue receive on error
        /* Check packet consistency */
        /* Проверяем соответствие количества байт и кода команды
        */

        /* Send REQ_ACK for "cmd" packets */
        // TODO: реализовать
        txBuf.

        /* Parse cmd packet, execute command and build tx packet */
        uint32_t regReadValOrWriteStatus;
        switch (rxBuf.FormatCmd.Cmd)
        {
        case CMD_READ:
            regReadValOrWriteStatus = reg_read( FreeRTOS_ntohl(rxBuf.FormatCmd.RegAddr) );
            // ID установили, когда отсылали ACK_REQ
            txBuf.FormatCmd.RDataOrWStatus = FreeRTOS_htonl(regReadValOrWriteStatus);
            break;

        case CMD_WRITE:
            regReadValOrWriteStatus = reg_write(FreeRTOS_ntohl(rxBuf.FormatCmd.RegAddr),
                FreeRTOS_ntohl(rxBuf.FormatCmd.ValueToWrite));
            // ID установили, когда отсылали ACK_REQ
            txBuf.FormatCmd.RDataOrWStatus = FreeRTOS_htonl(regReadValOrWriteStatus);
            break;

        default:
            configASSERT(pdFALSE);
            break;
        }




    }
}
/*-----------------------------------------------------------*/
void task2UDPConnection(void* pvParameters)
{

}
/*-----------------------------------------------------------*/
