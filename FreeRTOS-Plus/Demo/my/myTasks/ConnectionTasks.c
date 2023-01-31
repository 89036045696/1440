
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

// TODO: include прототипы reg_read, reg_write


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

//------------------------------------------------------------------------------
// Возможные значения поля "команда" принимаемого и отправляемого пакета.
//------------------------------------------------------------------------------

#define CMD_READ   ((uint8_t)  0)
#define CMD_WRITE  ((uint8_t)  1)
#define CMD_ACK    ((uint8_t)  2)

//------------------------------------------------------------------------------
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
// состояния обработки одного сообщения (транзакции)
enum TagExchangeStatesIds
{
    ID_EXCHANGE_STATE__NONE               = 0,      // означает, что выходной буфер "свободен"
    ID_EXCHANGE_STATE__NEED_TX_DATA_FIRST,          // после приёма пакета с командой, ответный пакет уже сформирован - отправить исходящий пакет
    ID_EXCHANGE_STATE__WAIT_ACK_REPLY,              // инициирована отправка ответного сообщения и происходит ожидание ответа ACK_REPLY - нужно отсчитывать время тайм-аута
    ID_EXCHANGE_STATE__NEED_TX_DATA_REPEAT,         // истёк тайм-аут ожидания ответа ACK_REPLY и количество повторных ответов не истекло, нужно повторить ответ: ответный пакет уже сформирован - отправить исходящий пакет
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
void StartToTransmit( const struct TagTxContext * ArgPtrContext )
{
    // TODO: мониторить время блокировки в реальных условиях и если оно велико, то либо увеличивать размер очереди, либо отменять ответ
    while (pdTRUE != xQueueSendToBack(QueueSendHandle, ArgPtrContext, 0)) { ; }
}
//==============================================================================

void task1UDPConnection( void *pvParameters )
{
    QueueSendHandle = xQueueCreate(SETTING__NUM_PACKETS_MAX, sizeof(struct TagTxContext));

    union TagRxBuffer         rxBuf;
    
    //---------------------------------------
    // ассоциированы с буферами отправки
    //---------------------------------------

    struct TagTxContext       txContexts[SETTING__NUM_PACKETS_MAX];
    struct TagTxContext       txContextAckReq;
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

    txContextAckReq.Packet.FormatAckRequest.AckRequest = CMD_ACK; // значение далее меняться не будет
    txContextAckReq.SizeOfPacket = sizeof( txContextAckReq.Packet.FormatAckRequest );

    // RX blocking timeout for all RX = 0. Later we won't overwrite it
    const TickType_t rxTimeOut = 0;
    FreeRTOS_setsockopt(((struct TagParamsOfUDPConnectionTask*)pvParameters)->ClientSocket,
        0,
        FREERTOS_SO_RCVTIMEO,
        &rxTimeOut,
        sizeof(rxTimeOut) );

    int32_t rxNum;
    while (1)
    {
        rxNum = FreeRTOS_recvfrom(((struct TagParamsOfUDPConnectionTask*)pvParameters)->ClientSocket,
            &rxBuf, sizeof(rxBuf) /* = максимальный размер входящего пакета */,
            0 /* ulFlags with the FREERTOS_ZERO_COPY bit clear. */,
            ((struct TagParamsOfUDPConnectionTask*)pvParameters)->PtrDestinationAddress,
            & ((struct TagParamsOfUDPConnectionTask*)pvParameters)->SourceAddressLength /* Not used but should be set as shown. */
        );
        if (rxNum > 0)
        { // если успешно приняты данные
            /* Check packet consistency */
            /* Проверяем соответствие количества байт и кода команды */
            if (rxBuf.FormatAckReply.Cmd == CMD_ACK && rxNum == sizeof(rxBuf.FormatAckReply))
            { //----------- пакет ACK_REPLY -----------
                // Ищем такой же Id в буферах отправки
                uint8_t idOfTxBuf = FindTransactionIdInArray(rxBuf.FormatAckReply.Id, txContexts, SETTING__NUM_PACKETS_MAX);
                if (idOfTxBuf < SETTING__NUM_PACKETS_MAX)
                { // найден индекс выходного буфера, для которого пришло подтверждение
                    if (states[idOfTxBuf] == ID_EXCHANGE_STATE__WAIT_ACK_REPLY)
                    {
                        states[idOfTxBuf] = ID_EXCHANGE_STATE__NONE; // транзакция успешно завершена, буфер можно освободить
                        // TODO: это нужно?                    configASSERT(numPacketsInProcessing-- != 0);
                    }
                    else { /* тихо игнорируем входящий пакет */ }
                }
                else { /* тихо игнорируем входящий пакет */ }
            }
            else if (rxBuf.FormatCmdRead.Cmd == CMD_READ && rxNum == sizeof(rxBuf.FormatCmdRead))
            { //----------- пакет READ -----------
                //------------------------
                // Отправка ACK_REQ
                //------------------------
                txContextAckReq.Packet.FormatAckRequest.Id = rxBuf.FormatAckReply.Id;
                // остальные поля структуры были инициализированы и не меняются
                StartToTransmit( &txContextAckReq );
                //------------------------
                // Ищем такой же Id в буферах отправки
                uint8_t idOfTxBuf = FindTransactionIdInArray(rxBuf.FormatAckReply.Id, txContexts, SETTING__NUM_PACKETS_MAX);
                if (idOfTxBuf < SETTING__NUM_PACKETS_MAX)
                { // Найден индекс выходного буфера, в котором уже подготовлен ответ: значит, повторный пакет (команда ранее была выполнена и пакет собран). 
                    states[idOfTxBuf] = ID_EXCHANGE_STATE__NEED_TX_DATA_FIRST;
                }
                else
                { // значит, вновь входящий пакет
                    // поиск свободного выходного буфера для формирования ответа
                    uint8_t idOfTxBufToConstruct = FindExchangeStateInArray(ID_EXCHANGE_STATE__NONE, states, SETTING__NUM_PACKETS_MAX);
                    if (idOfTxBufToConstruct < SETTING__NUM_PACKETS_MAX)
                    { // найден индекс свободного выходного буфера
                        // выполнение команды и сборка выходного пакета
                        uint32_t regReadVal = reg_read(FreeRTOS_ntohl(rxBuf.FormatCmdRead.RegAddr));
                        txContexts[idOfTxBufToConstruct].Packet.FormatCmdAnswer.Id = rxBuf.FormatCmdRead.Id;
                        txContexts[idOfTxBufToConstruct].Packet.FormatCmdAnswer.Cmd = rxBuf.FormatCmdRead.Cmd;
                        txContexts[idOfTxBufToConstruct].Packet.FormatCmdAnswer.RDataOrWStatus = FreeRTOS_htonl(regReadVal);
                        txContexts[idOfTxBufToConstruct].SizeOfPacket = sizeof(txContexts[idOfTxBufToConstruct].Packet.FormatCmdAnswer);

                        states[idOfTxBufToConstruct] = ID_EXCHANGE_STATE__NEED_TX_DATA_FIRST;
                    }
                    else { /* тихо игнорируем входящий пакет */ }
                }
            }
            else if (rxBuf.FormatCmdWrite.Cmd == CMD_WRITE && rxNum == sizeof(rxBuf.FormatCmdWrite))
            { //----------- пакет WRITE -----------
                //------------------------
                // Отправка ACK_REQ
                //------------------------
                txContextAckReq.Packet.FormatAckRequest.Id = rxBuf.FormatAckReply.Id;
                // остальные поля структуры были инициализированы и не меняются
                StartToTransmit(&txContextAckReq);
                //------------------------
                // Ищем такой же Id в буферах отправки
                uint8_t idOfTxBuf = FindTransactionIdInArray(rxBuf.FormatAckReply.Id, txContexts, SETTING__NUM_PACKETS_MAX);
                if (idOfTxBuf < SETTING__NUM_PACKETS_MAX)
                { // значит, повторный пакет. Найден индекс выходного буфера, в котором уже подготовлен ответ
                    states[idOfTxBuf] = ID_EXCHANGE_STATE__NEED_TX_DATA_FIRST;
                }
                else
                { // значит, вновь входящий пакет
                    // поиск свободного выходного буфера для формирования ответа
                    uint8_t idOfTxBufToConstruct = FindExchangeStateInArray(ID_EXCHANGE_STATE__NONE, states, SETTING__NUM_PACKETS_MAX);
                    if (idOfTxBufToConstruct < SETTING__NUM_PACKETS_MAX)
                    { // найден индекс
                        // выполнение команды и сборка выходного пакета
                        uint32_t regWriteStatus = reg_write(FreeRTOS_ntohl(rxBuf.FormatCmdWrite.RegAddr, rxBuf.FormatCmdWrite.ValueToWrite));
                        txContexts[idOfTxBufToConstruct].Packet.FormatCmdAnswer.Id = rxBuf.FormatCmdWrite.Id;
                        txContexts[idOfTxBufToConstruct].Packet.FormatCmdAnswer.Cmd = rxBuf.FormatCmdWrite.Cmd;
                        txContexts[idOfTxBufToConstruct].Packet.FormatCmdAnswer.RDataOrWStatus = FreeRTOS_htonl(regWriteStatus);
                        txContexts[idOfTxBufToConstruct].SizeOfPacket = sizeof(txContexts[idOfTxBufToConstruct].Packet.FormatCmdAnswer);

                        states[idOfTxBufToConstruct] = ID_EXCHANGE_STATE__NEED_TX_DATA_FIRST;
                    }
                    else { /* тихо игнорируем входящий пакет */ }
                }
            }
            else { /* считаем что неизвестной команды быть не может */ }
        }
        else { /* при ошибке приёма ничего не делаем */ }

        /****************************************/
        // Проверка истечения тайм-аута неответа для буферов c соотв. статусом

        TickType_t t = xTaskGetTickCount();
        for (size_t i = 0; i < SETTING__NUM_PACKETS_MAX; i++)
        {
            if (states[i] == ID_EXCHANGE_STATE__WAIT_ACK_REPLY)
            {
                if ( t - momentsOfTxStartLast[i] <= TIMESPAN_WAIT_ACK_REPLY )
                {
                    if (numsOfFailAckReply[i] < NUM_OF_FAIL_ACK_REPLY_MAX)
                    {
                        numsOfFailAckReply[i]++;
                        states[i] = ID_EXCHANGE_STATE__NEED_TX_DATA_REPEAT;
                    }
                    else
                    {
                        /* Если закончился последний тайм-аут неответа ACK_REPLY, то освобождаем выходной буфер */
                        states[i] = ID_EXCHANGE_STATE__NONE;
                        // TODO: это нужно?                    configASSERT(numPacketsInProcessing-- != 0);
                    }
                }
            }
        }

        /****************************************/
        /* Отправка пакета данных для буферов с соотв. статусом */

        for (size_t i = 0; i < SETTING__NUM_PACKETS_MAX; ++i)
        {
            if (states[i] == ID_EXCHANGE_STATE__NEED_TX_DATA_FIRST)
            {
                StartToTransmit(&txContexts[i]);
                numsOfFailAckReply[i] = 0;
                states[i] = ID_EXCHANGE_STATE__WAIT_ACK_REPLY;
            }
            else if (states[i] == ID_EXCHANGE_STATE__NEED_TX_DATA_REPEAT)
            {
                StartToTransmit(&txContexts[i]);
                states[i] = ID_EXCHANGE_STATE__WAIT_ACK_REPLY;
            }
        }
    } // end while(1)
}
//==============================================================================
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

    struct TagTxContext txContext;

    while (1)
    {
        while (pdTRUE != xQueueReceive( QueueSendHandle, &txContext, portMAX_DELAY)) { ; }

        FreeRTOS_sendto(((struct TagParamsOfUDPConnectionTask*)pvParameters)->ClientSocket,
            &(txContext.Packet),
            txContext.SizeOfPacket /*TODO: размер пакета передавать в очереди*/,
            0 /* ulFlags with the FREERTOS_ZERO_COPY bit clear. */,
            ((struct TagParamsOfUDPConnectionTask*)pvParameters)->PtrDestinationAddress,
            sizeof(struct freertos_sockaddr)
        );
    }
}
//==============================================================================
