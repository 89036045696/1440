
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


/*-----------------------------------------------------------*/

QueueHandle_t QueueSendHandle;

/*******************************************************************************
* Возможные значения поля "команда" принимаемого пакета с командой.
*******************************************************************************/

#define CMD_READ  ((uint8_t)  0)
#define CMD_WRITE  ((uint8_t)  1)
#define ACK_REPLY  ((uint8_t)  2)

const uint8_t ACK_REQ = 1;

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
        uint8_t AckReply;
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

    struct TagTxBufFormatCmdAnswer
    {
        uint8_t Id;
        uint32_t RDataOrWStatus;      // network endian
    } FormatCmdAnswer;

    struct TagTxBufFormatAckRequest
    {
        uint8_t Id;
        uint8_t AckRequest;
    } FormatAckRequest;
};
#ifdef WIN32
// restore default packing
#pragma pack(pop)
#endif


void task1UDPConnection( void *pvParameters )
{
    QueueSendHandle = xQueueCreate(1, sizeof(union TagTxBuffer));

    union TagRxBuffer rxBuf;
    union TagTxBuffer txBuf;
    while (1)
    {
        /* Read new RX packet with command */

        // RX blocking time = infinity
        const TickType_t rxTimeOut = portMAX_DELAY;
        FreeRTOS_setsockopt(((struct TagParamsOfUDPConnectionTask*)pvParameters)->ClientSocket,
            0,
            FREERTOS_SO_RCVTIMEO,
            &rxTimeOut,
            sizeof(rxTimeOut));

        int32_t rxNum = FreeRTOS_recvfrom(((struct TagParamsOfUDPConnectionTask*)pvParameters)->ClientSocket,
            &rxBuf, sizeof(rxBuf),
            0 /* ulFlags with the FREERTOS_ZERO_COPY bit clear. */,
            ((struct TagParamsOfUDPConnectionTask*)pvParameters)->PtrDestinationAddress,
            & ((struct TagParamsOfUDPConnectionTask*)pvParameters)->SourceAddressLength /* Not used but should be set as shown. */
        );
        if (rxNum <= 0) { continue; } // continue receive on error
        /* Check packet consistency */
        /* Проверяем соответствие количества байт и кода команды
        */
        if (rxBuf.FormatCmdRead.Cmd == CMD_READ && rxNum == sizeof(rxBuf.FormatCmdRead)) { ; /* ok */ }
        else if (rxBuf.FormatCmdWrite.Cmd == CMD_WRITE && rxNum == sizeof(rxBuf.FormatCmdWrite)) { ; /* ok */ }
        else
        {
            continue; // Игнорируем и продолжаем принимать, т.к. других сообщений на этом этапе быть не должно.
        }

        /* Send REQ_ACK for "cmd" packets */
        txBuf.FormatAckRequest.Id = rxBuf.FormatCmdRead.Id;
        txBuf.FormatAckRequest.AckRequest = ACK_REQ;
        // TODO: 
        while (pdTRUE != xQueueSendToBack(QueueSendHandle, &txBuf, 1)) { ; }

        /* Parse cmd packet, execute command and build tx packet */
        uint32_t regReadValOrWriteStatus;
        switch (rxBuf.FormatCmdRead.Cmd)
        {
        case CMD_READ:
            regReadValOrWriteStatus = reg_read( FreeRTOS_ntohl(rxBuf.FormatCmdRead.RegAddr) );
            // ID установили, когда отсылали ACK_REQ
            txBuf.FormatCmdAnswer.RDataOrWStatus = FreeRTOS_htonl(regReadValOrWriteStatus);
            break;

        case CMD_WRITE:
            regReadValOrWriteStatus = reg_write(FreeRTOS_ntohl(rxBuf.FormatCmdWrite.RegAddr),
                FreeRTOS_ntohl(rxBuf.FormatCmdWrite.ValueToWrite));
            // ID установили, когда отсылали ACK_REQ
            txBuf.FormatCmdAnswer.RDataOrWStatus = FreeRTOS_htonl(regReadValOrWriteStatus);
            break;
        }

        /****************************************/
        /* Send Tx packet and receive REPLY_ACK */

        // max send + answer timeout => RX blocking time
        const TickType_t rxReplyAckTimeOut = pdMS_TO_TICKS(1);
        FreeRTOS_setsockopt(((struct TagParamsOfUDPConnectionTask*)pvParameters)->ClientSocket,
            0,
            FREERTOS_SO_RCVTIMEO,
            &rxReplyAckTimeOut,
            sizeof(rxReplyAckTimeOut));

        for (int8_t nRepeats = 10; nRepeats > 0; nRepeats--)
        {
            // TODO: 
            while (pdTRUE != xQueueSendToBack(QueueSendHandle, &txBuf, 1)) { ; }

            int32_t rxNum = FreeRTOS_recvfrom(((struct TagParamsOfUDPConnectionTask*)pvParameters)->ClientSocket,
                &rxBuf, sizeof(rxBuf.FormatAckReply),
                0 /* ulFlags with the FREERTOS_ZERO_COPY bit clear. */,
                ((struct TagParamsOfUDPConnectionTask*)pvParameters)->PtrDestinationAddress,
                &((struct TagParamsOfUDPConnectionTask*)pvParameters)->SourceAddressLength /* Not used but should be set as shown. */
            );

            if (rxNum != sizeof(rxBuf.FormatAckReply)) { continue; }
            if (rxBuf.FormatAckReply.Id != txBuf.FormatCmdAnswer.Id) { continue; }
            if (rxBuf.FormatAckReply.AckReply != ACK_REPLY) { continue; }

            break;
        }

    }
}
/*-----------------------------------------------------------*/
void task2UDPConnection(void* pvParameters)
{
    while (QueueSendHandle == NULL) { ; }

    union TagTxBuffer txBuf;
    while (1)
    {
        while (pdTRUE != xQueueReceive( QueueSendHandle, &txBuf, portMAX_DELAY)) { ; }


        size_t sendLength = txBuf.bytes[1] == ACK_REQ ? sizeof(txBuf.FormatAckRequest) :
            sizeof(txBuf.FormatCmdAnswer);
        FreeRTOS_sendto(((struct TagParamsOfUDPConnectionTask*)pvParameters)->ClientSocket,
            &txBuf,
            txBuf.bytes[1] == ACK_REQ ? sizeof(txBuf.FormatAckRequest) : sizeof(txBuf.FormatCmdAnswer),
            0 /* ulFlags with the FREERTOS_ZERO_COPY bit clear. */,
            ((struct TagParamsOfUDPConnectionTask*)pvParameters)->PtrDestinationAddress,
            sizeof(struct freertos_sockaddr)
        );
    }
}
/*-----------------------------------------------------------*/
