
/* Standard includes. */
#include <stdint.h>
#include <stdbool.h>
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
        uint8_t Cmd;
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
    uint8_t Bytes[6];

    struct TagTxBufFormatCmdAnswer
    {
        uint8_t Id;
        uint8_t Cmd;
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
    ID_EXCHANGE_STATE__NEED_TX,                     // ответный пакет уже сформирован - отправить исходящий пакет
    ID_EXCHANGE_STATE__WAIT_ACK_REPLY,              // инициирована отправка ответного сообщения и происходит ожидание ответа ACK_REPLY - нужно отсчитывать время тайм-аута
    ID_EXCHANGE_STATE__NEED_REPEAT_REPLY,           // истёк тайм-аут ожидания ответа ACK_REPLY и количество повторных ответов не истекло, нужно повторить ответ: ответный пакет уже сформирован - отправить исходящий пакет
};

//------------------------------------------------------------------------------

struct TagTxContext
{
    union TagTxBuffer    Packet;
    uint8_t              SizeOfPacket;
};


//==============================================================================
//==============================================================================
// TODO: вместо этой ф-ии для оптимальности можно держать массив счётчиков для состояний, но пусть так сделает кому нужна бОльшая оптимальность ;)
uint8_t CountExchangeStateInArray(enum TagExchangeStatesIds  ArgState, enum TagExchangeStatesIds* ArgArrayStates, uint8_t ArgArrLen)
{
    uint8_t retVal = 0;
    for (uint8_t i = 0; i < ArgArrLen; ++i)
    {
        if (ArgArrayStates[i] == ArgState)
        {
            retVal++;
        }
    }
    return retVal;
}
//==============================================================================
uint8_t FindExchangeStateInArray(enum TagExchangeStatesIds  ArgState, enum TagExchangeStatesIds* ArgArrayStates, uint8_t ArgArrLen)
{
    for (uint8_t i = 0; i < ArgArrLen; ++i)
    {
        if (ArgArrayStates[i] == ArgState)
        {
            return i;
        }
    }
    return ArgArrLen;
}
//==============================================================================
uint8_t FindTransactionIdInArray(uint8_t  ArgId, struct TagTxContext* ArgArrTxItems, uint8_t ArgArrLen)
{
    for (uint8_t i = 0; i < ArgArrLen; ++i)
    {
        if (ArgArrTxItems[i].Packet.FormatCmdAnswer.Id == ArgId)
        {
            return i;
        }
    }
    return ArgArrLen;
}
//==============================================================================
/**
 \return - true: данные приняты в буфер \arg ArgPtrBuf
*/
bool TryToReceive(struct TagParamsOfUDPConnectionTask* ArgParams, union TagRxBuffer* ArgPtrBuf)
{
    // RX blocking time = 0
    const TickType_t rxTimeOut = 0;
    FreeRTOS_setsockopt(ArgParams->ClientSocket,
        0,
        FREERTOS_SO_RCVTIMEO,
        &rxTimeOut,
        sizeof(rxTimeOut));

    int32_t rxNum = FreeRTOS_recvfrom(ArgParams->ClientSocket,
        ArgPtrBuf, sizeof(union TagRxBuffer) /* = максимальный размер входящего пакета */,
        0 /* ulFlags with the FREERTOS_ZERO_COPY bit clear. */,
        ArgParams->PtrDestinationAddress,
        &ArgParams->SourceAddressLength /* Not used but should be set as shown. */
    );

    return rxNum > 0;
}
//==============================================================================



//==============================================================================

void task1UDPConnection( void *pvParameters )
{
    QueueSendHandle = xQueueCreate(SETTING__NUM_PACKETS_MAX, sizeof(struct TagTxContext));

    union TagRxBuffer         rxBuf;
    
    //---------------------------------------
    // ассоциированы с буферами отправки
    //---------------------------------------

    struct TagTxContext       txItems[SETTING__NUM_PACKETS_MAX];
    struct TagTxContext       txItemAckReq;
    enum TagExchangeStatesIds states[SETTING__NUM_PACKETS_MAX];
    TickType_t                momentsOfTxStartLast[SETTING__NUM_PACKETS_MAX]; // (ticks), для простоты начинаем отсчитывать время локально, а не в задаче отправки.
    uint8_t                   numsOfFailAckReply[SETTING__NUM_PACKETS_MAX]; // TODO: инициализировать и изменять в коде
    //---------------------------------------
// TODO: это нужно?    uint8_t                   numPacketsInProcessing; // макс. количество находящихся в обработке уникальных входящих пакетов
    //---------------------------------------
    static const TickType_t   TIMESPAN_WAIT_ACK_REPLY = pdMS_TO_TICKS(1);/*setting*/
    static const TickType_t   NUM_OF_FAIL_ACK_REPLY_MAX = 10;/*setting*/

    for (size_t i = 0; i < SETTING__NUM_PACKETS_MAX; ++i)
    {
        states[i] = ID_EXCHANGE_STATE__NONE;
    }
    // TODO: это нужно?    numPacketsInProcessing = 0;

    txItemAckReq.Packet.FormatAckRequest.AckRequest = CMD_ACK; // значение далее меняться не будет

    // RX blocking timeout for all RX = 0. Later we won't overwrite it
    const TickType_t rxTimeOut = 0;
    FreeRTOS_setsockopt(((struct TagParamsOfUDPConnectionTask*)pvParameters)->ClientSocket,
        0,
        FREERTOS_SO_RCVTIMEO,
        &rxTimeOut,
        sizeof(rxTimeOut) );

    while (1)
    {

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
            /* Проверяем соответствие количества байт и кода команды */
            //----------- пакет ACK_REPLY -----------
            if (rxBuf.FormatAckReply.Cmd == CMD_ACK && rxNum == sizeof(rxBuf.FormatAckReply))
            { // пакет ACK_REPLY
                // Ищем такой же Id в буферах отправки
                uint8_t idOfTxBuf = FindTransactionIdInArray(rxBuf.FormatAckReply.Id, txItems, SETTING__NUM_PACKETS_MAX);
                if (idOfTxBuf < SETTING__NUM_PACKETS_MAX)
                { // найден индекс выходного буфера, для которого пришло подтверждение
                    if (states[idOfTxBuf] == ID_EXCHANGE_STATE__WAIT_ACK_REPLY)
                    {
                        states[idOfTxBuf] = ID_EXCHANGE_STATE__NONE; // транзакция успешно завершена, буфер можно освободить
                        // TODO: это нужно?                    configASSERT(numPacketsInProcessing-- != 0);
                    }
                    else
                    { /* тихо игнорируем входящий пакет */
                    }
                }
                else
                { /* тихо игнорируем входящий пакет */
                }
            }
            //----------- пакет READ -----------
            else if (rxBuf.FormatCmdRead.Cmd == CMD_READ && rxNum == sizeof(rxBuf.FormatCmdRead))
            {
                // Ищем такой же Id в буферах отправки
                uint8_t idOfTxBuf = FindTransactionIdInArray(rxBuf.FormatAckReply.Id, txItems, SETTING__NUM_PACKETS_MAX);
                if (idOfTxBuf < SETTING__NUM_PACKETS_MAX)
                { // значит, повторный пакет. Найден индекс выходного буфера, в котором уже подготовлен ответ
                    //------------------------
                    // Отправка ACK_REQ
                    txItemAckReq.Packet.FormatAckRequest.Id = rxBuf.FormatAckReply.Id;
                    while (pdTRUE != xQueueSendToBack(QueueSendHandle, &txItemAckReq, 1)) { ; }
                    //------------------------
                    states[idOfTxBuf] = ID_EXCHANGE_STATE__NEED_TX;
                }
                else
                { // значит, вновь входящий пакет
                    // поиск свободного выходного буфера для формирования ответа
                    uint8_t idOfTxBufToConstruct = FindExchangeStateInArray(ID_EXCHANGE_STATE__NONE, states, SETTING__NUM_PACKETS_MAX);
                    if (idOfTxBufToConstruct < SETTING__NUM_PACKETS_MAX)
                    { // найден индекс
                        //------------------------
                        // Отправка ACK_REQ
                        txItemAckReq.Packet.FormatAckRequest.Id = rxBuf.FormatCmdRead.Id;
                        while (pdTRUE != xQueueSendToBack(QueueSendHandle, &txItemAckReq, 1)) { ; }
                        //------------------------
                        // выполнение команды и сборка выходного пакета
                        uint32_t regReadVal = reg_read(FreeRTOS_ntohl(rxBuf.FormatCmdRead.RegAddr));
                        txItems[idOfTxBufToConstruct].Packet.FormatCmdAnswer.Id = rxBuf.FormatCmdRead.Id;
                        txItems[idOfTxBufToConstruct].Packet.FormatCmdAnswer.Cmd = rxBuf.FormatCmdRead.Cmd;
                        txItems[idOfTxBufToConstruct].Packet.FormatCmdAnswer.RDataOrWStatus = FreeRTOS_htonl(regReadVal);
                        states[idOfTxBufToConstruct] = ID_EXCHANGE_STATE__NEED_TX;
                    }
                    else
                    { /* тихо игнорируем входящий пакет */
                    }
                }
            }
            //----------- пакет WRITE -----------
            else if (rxBuf.FormatCmdWrite.Cmd == CMD_WRITE && rxNum == sizeof(rxBuf.FormatCmdWrite))
            {
                // Ищем такой же Id в буферах отправки
                uint8_t idOfTxBuf = FindTransactionIdInArray(rxBuf.FormatAckReply.Id, txItems, SETTING__NUM_PACKETS_MAX);
                if (idOfTxBuf < SETTING__NUM_PACKETS_MAX)
                { // значит, повторный пакет. Найден индекс выходного буфера, в котором уже подготовлен ответ
                    //------------------------
                    // Отправка ACK_REQ
                    txItemAckReq.Packet.FormatAckRequest.Id = rxBuf.FormatAckReply.Id;
                    while (pdTRUE != xQueueSendToBack(QueueSendHandle, &txItemAckReq, 1)) { ; }
                    //------------------------
                    states[idOfTxBuf] = ID_EXCHANGE_STATE__NEED_TX;
                }
                else
                { // значит, вновь входящий пакет
                    // поиск свободного выходного буфера для формирования ответа
                    uint8_t idOfTxBufToConstruct = FindExchangeStateInArray(ID_EXCHANGE_STATE__NONE, states, SETTING__NUM_PACKETS_MAX);
                    if (idOfTxBufToConstruct < SETTING__NUM_PACKETS_MAX)
                    { // найден индекс
                        //------------------------
                        // Отправка ACK_REQ
                        txItemAckReq.Packet.FormatAckRequest.Id = rxBuf.FormatCmdWrite.Id;
                        while (pdTRUE != xQueueSendToBack(QueueSendHandle, &txItemAckReq, 1)) { ; }
                        //------------------------
                        // выполнение команды и сборка выходного пакета
                        uint32_t regWriteStatus = reg_write(FreeRTOS_ntohl(rxBuf.FormatCmdWrite.RegAddr, rxBuf.FormatCmdWrite.ValueToWrite));
                        txItems[idOfTxBufToConstruct].Packet.FormatCmdAnswer.Id = rxBuf.FormatCmdWrite.Id;
                        txItems[idOfTxBufToConstruct].Packet.FormatCmdAnswer.Cmd = rxBuf.FormatCmdWrite.Cmd;
                        txItems[idOfTxBufToConstruct].Packet.FormatCmdAnswer.RDataOrWStatus = FreeRTOS_htonl(regWriteStatus);
                        states[idOfTxBufToConstruct] = ID_EXCHANGE_STATE__NEED_TX;
                    }
                    else
                    { /* тихо игнорируем входящий пакет */ }
                }
            }
            else
            { /* считаем что неизвестной команды быть не может */ }
        }
        else
        { /* при ошибке приёма ничего не делаем */ }

        /****************************************/
        // Проверка истечения тайм-аута неответа для буферов со статусом ID_EXCHANGE_STATE__WAIT_ACK_REPLY
        TickType_t t = GetTickCount();
        for (size_t i = 0; i < SETTING__NUM_PACKETS_MAX; i++)
        {
            if (states[i] == ID_EXCHANGE_STATE__WAIT_ACK_REPLY)
            {
                if (t - momentsOfTxStartLast[i] >= TIMESPAN_WAIT_ACK_REPLY)
                {
                    numsOfFailAckReply[i]++;
                    if (numsOfFailAckReply[i] < NUM_OF_FAIL_ACK_REPLY_MAX)
                    {
                        states[i] = ID_EXCHANGE_STATE__NEED_REPEAT_REPLY;
                    }
                    else
                    {
                        /* Если закончился последний тайм-аут неответа ACK_REPLY,
                         то меняем статус на ...NONE (освобождаем выходной буфер)
                        */
                        states[i] = ID_EXCHANGE_STATE__NONE;
                        // TODO: это нужно?                    configASSERT(numPacketsInProcessing-- != 0);
                    }

                }
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
            while (pdTRUE != xQueueSendToBack(QueueSendHandle, &txItems[TODO: idOfTxBuf], 1)) { ; }

            int32_t rxNum = FreeRTOS_recvfrom(((struct TagParamsOfUDPConnectionTask*)pvParameters)->ClientSocket,
                &rxBuf, sizeof(rxBuf.FormatAckReply),
                0 /* ulFlags with the FREERTOS_ZERO_COPY bit clear. */,
                ((struct TagParamsOfUDPConnectionTask*)pvParameters)->PtrDestinationAddress,
                &((struct TagParamsOfUDPConnectionTask*)pvParameters)->SourceAddressLength /* Not used but should be set as shown. */
            );

            if (rxNum != sizeof(rxBuf.FormatAckReply)) { continue; }
            if (rxBuf.FormatAckReply.Id != txBuf.FormatCmdAnswer.Id) { continue; }
            if (rxBuf.FormatAckReply.Cmd != CMD_ACK) { continue; }

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

    struct TagTxContext txItem;
    while (1)
    {
        while (pdTRUE != xQueueReceive( QueueSendHandle, &txItem, portMAX_DELAY)) { ; }

        FreeRTOS_sendto(((struct TagParamsOfUDPConnectionTask*)pvParameters)->ClientSocket,
            &(txItem.Packet),
            txItem.SizeOfPacket /*TODO: размер пакета передавать в очереди*/,
            0 /* ulFlags with the FREERTOS_ZERO_COPY bit clear. */,
            ((struct TagParamsOfUDPConnectionTask*)pvParameters)->PtrDestinationAddress,
            sizeof(struct freertos_sockaddr)
        );
    }
}
//==============================================================================
