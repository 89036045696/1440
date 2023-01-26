
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
* Возможные значения поля "команда" принимаемого и отправляемого пакета.
*******************************************************************************/

#define CMD_READ   ((uint8_t)  0)
#define CMD_WRITE  ((uint8_t)  1)
#define CMD_ACK    ((uint8_t)  2)

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

enum ExchangeStatesIds
{
    ID_EXCHANGE_STATE__NONE               = 0,
    ID_EXCHANGE_STATE__ACK_REQUEST,                 // инициирована отправка сообщения ACK_REQUEST
    ID_EXCHANGE_STATE__WAIT_ACK_REPLY,              // инициирована отправка ответного сообщения и происходит ожидание ответа ACK_REPLY
    ID_EXCHANGE_STATE__RECEIVED_REPEAT              // принято сообщение с повторным статусом (т.е. команду из пакета повторно выполнять не нужно и ответный пакет тоже уже сформирован - отправить ACk и исходящий пакет)
};


void task1UDPConnection( void *pvParameters )
{
    QueueSendHandle = xQueueCreate(1, sizeof(union TagTxBuffer));

    union TagRxBuffer rxBuf;
    union TagTxBuffer txBuf;
    while (1)
    {
        /* Перед новым приёмом 
        Поиск свободного места в приёмном буфере :
        если свободное место есть, то принимаем новый пакет
        если свободного места нет, то пропускаем шаг
        */

        /* Read new RX packet with command */

        // RX blocking time = 0
        const TickType_t rxTimeOut = 0;
        FreeRTOS_setsockopt(((struct TagParamsOfUDPConnectionTask*)pvParameters)->ClientSocket,
            0,
            FREERTOS_SO_RCVTIMEO,
            &rxTimeOut,
            sizeof(rxTimeOut));

        int32_t rxNum = FreeRTOS_recvfrom(((struct TagParamsOfUDPConnectionTask*)pvParameters)->ClientSocket,
            &rxBuf, sizeof(rxBuf) /* = максимальный размер входящего пакета */,
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
        txBuf.FormatAckRequest.AckRequest = CMD_ACK;
        // TODO: 
         /*TODO: размер пакета передавать в очереди*/
        while (pdTRUE != xQueueSendToBack(QueueSendHandle, &txBuf, 1)) { ; }

        /* Parse cmd packet, execute command and build tx packet */
        uint32_t regReadValOrWriteStatus;
        switch (rxBuf.FormatCmdRead.Cmd)
        {
        case CMD_READ:
            regReadValOrWriteStatus = reg_read( FreeRTOS_ntohl(rxBuf.FormatCmdRead.RegAddr) );
            // ID транзакции установили в буфере, когда отсылали ACK_REQ
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

        // TODO: переделать логику без блокировки
        for (int8_t nRepeats = 10; nRepeats > 0; nRepeats--)
        {
            /*TODO: размер пакета tx передавать в очереди*/
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
            if (rxBuf.FormatAckReply.AckReply != CMD_ACK) { continue; }

            break;
        }

    }
}
/*-----------------------------------------------------------*/
void task2UDPConnection(void* pvParameters)
{
    while (QueueSendHandle == NULL) { ; }

    // max sendto timeout
    const TickType_t txTimeOutMax = portMAX_DELAY;
    FreeRTOS_setsockopt(((struct TagParamsOfUDPConnectionTask*)pvParameters)->ClientSocket,
        0,
        FREERTOS_SO_SNDTIMEO,
        &txTimeOutMax,
        sizeof(txTimeOutMax));

    union TagTxBuffer txBuf;
    while (1)
    {
        while (pdTRUE != xQueueReceive( QueueSendHandle, &txBuf, portMAX_DELAY)) { ; }

        /*TODO: размер пакета tx передавать в очереди*/
        // Размер пакета отправляемых данных разный, и зависит от кода команды в нём
        size_t sendLength = txBuf.bytes[1] == CMD_ACK ? sizeof(txBuf.FormatAckRequest) :
            /* CMD_READ or CMD_WRITE */ sizeof(txBuf.FormatCmdAnswer);
        FreeRTOS_sendto(((struct TagParamsOfUDPConnectionTask*)pvParameters)->ClientSocket,
            &txBuf,
            sendLength /*TODO: размер пакета передавать в очереди*/,
            0 /* ulFlags with the FREERTOS_ZERO_COPY bit clear. */,
            ((struct TagParamsOfUDPConnectionTask*)pvParameters)->PtrDestinationAddress,
            sizeof(struct freertos_sockaddr)
        );
    }
}
/*-----------------------------------------------------------*/
