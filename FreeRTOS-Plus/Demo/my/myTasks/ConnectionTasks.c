
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

//------------------------------------------------------------------------------

#ifdef WIN32
#define PACKED
#else
// POSIX, ARDUINO
#define PACKED __attribute__((__packed__))
#endif

//------------------------------------------------------------------------------

#define SETTING__NUM_PACKETS_MAX  ((uint8_t) 8) // макс. количество находящихся в обработке уникальных входящих пакетов

//------------------------------------------------------------------------------

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
    } PACKED FormatCmdRead;

    struct TagRxBufFormatCmdWrite
    {
        uint8_t Id;
        uint8_t Cmd;
        uint32_t RegAddr;       // network endian
        uint32_t ValueToWrite;  // network endian
    } PACKED FormatCmdWrite;

    struct TagRxBufFormatAckReply
    {
        uint8_t Id;
        uint8_t AckReply;
    } PACKED FormatAckReply;
} PACKED;
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
    } PACKED FormatCmdAnswer;

    struct TagTxBufFormatAckRequest
    {
        uint8_t Id;
        uint8_t AckRequest;
    } PACKED FormatAckRequest;
} PACKED;
#ifdef WIN32
// restore default packing
#pragma pack(pop)
#endif
//------------------------------------------------------------------------------
// состояния обработки одного сообщения
enum TagExchangeStatesIds
{
    ID_EXCHANGE_STATE__NONE               = 0,
    ID_EXCHANGE_STATE__ACK_REQUEST,                 // инициирована отправка сообщения ACK_REQUEST
    ID_EXCHANGE_STATE__WAIT_ACK_REPLY,              // инициирована отправка ответного сообщения и происходит ожидание ответа ACK_REPLY
    ID_EXCHANGE_STATE__NEED_REPEAT_REPLY,           // истёк тайм-аут ожидания ответа ACK_REPLY и количество повторных ответов не истекло: нужно повторить ответ
    ID_EXCHANGE_STATE__RECEIVED_REPEAT              // принято сообщение с повторным статусом (т.е. команду из пакета повторно выполнять не нужно и ответный пакет тоже уже сформирован - отправить ACk и исходящий пакет)
};

//------------------------------------------------------------------------------

struct TagTxContext
{
    union TagTxBuffer    packet;
    uint8_t              sizeOfPacket;
};

//==============================================================================

void task1UDPConnection( void *pvParameters )
{
    QueueSendHandle = xQueueCreate(SETTING__NUM_PACKETS_MAX, sizeof(struct TagTxContext));

    union TagRxBuffer         rxBuf;
    
    //---------------------------------------
    // ассоциированы с буферами отправки
    //---------------------------------------

    struct TagTxContext       txItems[SETTING__NUM_PACKETS_MAX];
    enum TagExchangeStatesIds states[SETTING__NUM_PACKETS_MAX];
    TickType_t                momentsOfTxStartLast[SETTING__NUM_PACKETS_MAX]; // (ticks), для простоты начинаем отсчитывать время локально, а не в задаче отправки.
    uint8_t                   numsOfFailAckReply[SETTING__NUM_PACKETS_MAX];
    //---------------------------------------
    uint8_t                   numPacketsInProcessing; // макс. количество находящихся в обработке уникальных входящих пакетов
    //---------------------------------------
    static const TickType_t   TIMESPAN_WAIT_ACK_REPLY = pdMS_TO_TICKS(1);/*setting*/
    static const TickType_t   NUM_OF_FAIL_ACK_REPLY_MAX = 10;/*setting*/

    for (size_t i = 0; i < SETTING__NUM_PACKETS_MAX; ++i)
    {
        states[i] = ID_EXCHANGE_STATE__NONE;
    }
    numPacketsInProcessing = 0;

    // RX blocking timeout for all RX = 0. Later we won't overwrite it
    const TickType_t rxTimeOut = 0;
    FreeRTOS_setsockopt(((struct TagParamsOfUDPConnectionTask*)pvParameters)->ClientSocket,
        0,
        FREERTOS_SO_RCVTIMEO,
        &rxTimeOut,
        sizeof(rxTimeOut) );

    while (1)
    {
        /* Если закончился последний тайм-аут ожидания ACK_REPLY, то меняем статус на ...NONE (освобождаем выходной буфер)
        Поиск статуса ID_EXCHANGE_STATE__WAIT_ACK_REPLY для сравнения с макс. возможным таймаутом неответа */
        TickType_t t = GetTickCount();
        for ( size_t i = 0 ; i < SETTING__NUM_PACKETS_MAX; i++ )
        {
            if ( states[i] == ID_EXCHANGE_STATE__WAIT_ACK_REPLY )
            {
                if ( (t - momentsOfTxStartLast[i] >= TIMESPAN_WAIT_ACK_REPLY) &&
                     ( numsOfFailAckReply[i] >= NUM_OF_FAIL_ACK_REPLY_MAX )
                    )
                {
                    states[i] = ID_EXCHANGE_STATE__NONE;
                    configASSERT(numPacketsInProcessing-- == 0);
                }
            }
        }




        /* Read new RX packet */
        // + ничего не принято: критерий - (rxNum <= 0)
        // ------ ищем state == ID_EXCHANGE_STATE__NEED_REPEAT_REPLY, если нашли - переход к повторной отправке буфера, 
        // иначе - ищем state == ID_EXCHANGE_STATE__WAIT_ACK_REPLY, если нашли - переход к проверке истечения тайм-аута
        // 
        // - принято с логической ошибкой (неизвестный номер команды, количество байт не соответствует номеру команды) - считаем что такого быть не может
        // 
        // + пакет с данными: критерий - (cmd == CMD_READ) || (cmd == CMD_WRITE)
        // ++ повторно: критерий - (в выходном буфере есть такой же ID) - собрать ACK_REQ, отправить, собрать ответный пакет - перейти к след. этапу
        // ++ первично: критерий - (иначе) - выполнить, собрать ACK_REQ, отправить, собрать ответный пакет - перейти к след. этапу
        // 
        // - REPLY_ACK с логической ошибкой неправильным ID - такой ID не существует в нашем буфере - считаем что такого быть не может
        // + REPLY_ACK - если у вых. буфера с ID state == ID_EXCHANGE_STATE__WAIT_ACK_REPLY, то установить state = NONE
        // ----- если status == ID_EXCHANGE_STATE__WAIT_ACK_REPLY - то установить статус ID_EXCHANGE_STATE__NONE, иначе -  - перейти к след. этапу




        int32_t rxNum = FreeRTOS_recvfrom(((struct TagParamsOfUDPConnectionTask*)pvParameters)->ClientSocket,
            &rxBuf, sizeof(rxBuf) /* = максимальный размер входящего пакета */,
            0 /* ulFlags with the FREERTOS_ZERO_COPY bit clear. */,
            ((struct TagParamsOfUDPConnectionTask*)pvParameters)->PtrDestinationAddress,
            & ((struct TagParamsOfUDPConnectionTask*)pvParameters)->SourceAddressLength /* Not used but should be set as shown. */
        );
        if (rxNum > 0)
        { // если успешно приняты данные
            /* Check packet consistency */
            /* Проверяем соответствие количества байт и кода команды
            */
            if (rxBuf.FormatCmdRead.Cmd == CMD_READ && rxNum == sizeof(rxBuf.FormatCmdRead)) { ; /* ok */ }
            else if (rxBuf.FormatCmdWrite.Cmd == CMD_WRITE && rxNum == sizeof(rxBuf.FormatCmdWrite)) { ; /* ok */ }
            else
            {
                continue; // Игнорируем и продолжаем принимать, т.к. других сообщений на этом этапе быть не должно.
            }
			/* Перед формированием нового ответного пакета:
			Поиск свободного выходного буфера (критерий - статус):
            */
			uint8_t idOfFreeTxBuffer = 0; // если >= SETTING__NUM_PACKETS_MAX, то нет свободного буфера
			for (  ; idOfFreeTxBuffer < SETTING__NUM_PACKETS_MAX; ++idOfFreeTxBuffer)
			{
                if ( states[idOfFreeTxBuffer] == ID_EXCHANGE_STATE__NONE )
                {
					break; // в idOfFreeTxBuffer сохранится индекс
                }
            }
            /* Send REQ_ACK for "cmd" packets */
            txBuf.FormatAckRequest.Id = rxBuf.FormatCmdRead.Id;
            txBuf.FormatAckRequest.AckRequest = CMD_ACK;
            // TODO: 
             /*TODO: размер пакета передавать в очереди*/
            while (pdTRUE != xQueueSendToBack(QueueSendHandle, &txItem, 1)) { ; }

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
            while (pdTRUE != xQueueSendToBack(QueueSendHandle, &txItem, 1)) { ; }

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
//==============================================================================
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

    union TagTxContext txItem;
    while (1)
    {
        while (pdTRUE != xQueueReceive( QueueSendHandle, &txItem, portMAX_DELAY)) { ; }

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
//==============================================================================
