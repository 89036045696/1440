
/* Standard includes. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "task.h"

/* FreeRTOS+TCP includes. */
#include "FreeRTOS_Sockets.h"

// ��� �������� ������� ������
#include "FreeRTOS_IP.h"

// TODO: include ��������� reg_read, reg_write


#include "ConnectionTasks.h"


//------------------------------------------------------------------------------

#ifdef WIN32
#define PACKED
#else
// POSIX, ARDUINO
#define PACKED __attribute__((__packed__))
#endif

//------------------------------------------------------------------------------

#define SETTING__NUM_PACKETS_MAX  ((uint8_t) 8) // ����. ���������� ����������� � ��������� ���������� �������� �������

//------------------------------------------------------------------------------

QueueHandle_t QueueSendHandle;

//------------------------------------------------------------------------------
// ��������� �������� ���� "�������" ������������ � ������������� ������.
//------------------------------------------------------------------------------

#define CMD_READ   ((uint8_t)  0)
#define CMD_WRITE  ((uint8_t)  1)
#define CMD_ACK    ((uint8_t)  2)

//------------------------------------------------------------------------------
#ifdef WIN32
#pragma pack(push,1)
#endif
// ��������� ������� �������� �������
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
// ��������� ������� ��������� �������
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
// ��������� ��������� ������ ��������� (����������)
enum TagExchangeStatesIds
{
    ID_EXCHANGE_STATE__NONE               = 0,      // ��������, ��� �������� ����� "��������"
    ID_EXCHANGE_STATE__NEED_TX_DATA_FIRST,          // ����� ����� ������ � ��������, �������� ����� ��� ����������� - ��������� ��������� �����
    ID_EXCHANGE_STATE__WAIT_ACK_REPLY,              // ������������ �������� ��������� ��������� � ���������� �������� ������ ACK_REPLY - ����� ����������� ����� ����-����
    ID_EXCHANGE_STATE__NEED_TX_DATA_REPEAT,         // ���� ����-��� �������� ������ ACK_REPLY � ���������� ��������� ������� �� �������, ����� ��������� �����: �������� ����� ��� ����������� - ��������� ��������� �����
};

//------------------------------------------------------------------------------

struct TagTxContext
{
    union TagTxBuffer    Packet;
    uint8_t              SizeOfPacket;
};


//==============================================================================
//==============================================================================
// TODO: ������ ���� �-�� ��� ������������� ����� ������� ������ ��������� ��� ���������, �� ����� ��� ������� ���� ����� ������� ������������� ;)
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
 \return - true: ������ ������� � ����� \arg ArgPtrBuf
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
        ArgPtrBuf, sizeof(union TagRxBuffer) /* = ������������ ������ ��������� ������ */,
        0 /* ulFlags with the FREERTOS_ZERO_COPY bit clear. */,
        ArgParams->PtrDestinationAddress,
        &ArgParams->SourceAddressLength /* Not used but should be set as shown. */
    );

    return rxNum > 0;
}
//==============================================================================
void StartToTransmit( const struct TagTxContext * ArgPtrContext )
{
    // TODO: ���������� ����� ���������� � �������� �������� � ���� ��� ������, �� ���� ����������� ������ �������, ���� �������� �����
    while (pdTRUE != xQueueSendToBack(QueueSendHandle, ArgPtrContext, 0)) { ; }
}
//==============================================================================

void task1UDPConnection( void *pvParameters )
{
    QueueSendHandle = xQueueCreate(SETTING__NUM_PACKETS_MAX, sizeof(struct TagTxContext));

    union TagRxBuffer         rxBuf;
    
    //---------------------------------------
    // ������������� � �������� ��������
    //---------------------------------------

    struct TagTxContext       txContexts[SETTING__NUM_PACKETS_MAX];
    struct TagTxContext       txContextAckReq;
    enum TagExchangeStatesIds states[SETTING__NUM_PACKETS_MAX];
    TickType_t                momentsOfTxStartLast[SETTING__NUM_PACKETS_MAX]; // (ticks), ��� �������� �������� ����������� ����� ��������, � �� � ������ ��������.
    uint8_t                   numsOfFailAckReply[SETTING__NUM_PACKETS_MAX]; // TODO: ���������������� � �������� � ����
    //---------------------------------------
// TODO: ��� �����?    uint8_t                   numPacketsInProcessing; // ����. ���������� ����������� � ��������� ���������� �������� �������
    //---------------------------------------
    static const TickType_t   TIMESPAN_WAIT_ACK_REPLY = pdMS_TO_TICKS(1);/*setting*/
    static const TickType_t   NUM_OF_FAIL_ACK_REPLY_MAX = 10;/*setting*/

    for (size_t i = 0; i < SETTING__NUM_PACKETS_MAX; ++i)
    {
        states[i] = ID_EXCHANGE_STATE__NONE;
    }
    // TODO: ��� �����?    numPacketsInProcessing = 0;

    txContextAckReq.Packet.FormatAckRequest.AckRequest = CMD_ACK; // �������� ����� �������� �� �����
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
            &rxBuf, sizeof(rxBuf) /* = ������������ ������ ��������� ������ */,
            0 /* ulFlags with the FREERTOS_ZERO_COPY bit clear. */,
            ((struct TagParamsOfUDPConnectionTask*)pvParameters)->PtrDestinationAddress,
            & ((struct TagParamsOfUDPConnectionTask*)pvParameters)->SourceAddressLength /* Not used but should be set as shown. */
        );
        if (rxNum > 0)
        { // ���� ������� ������� ������
            /* Check packet consistency */
            /* ��������� ������������ ���������� ���� � ���� ������� */
            if (rxBuf.FormatAckReply.Cmd == CMD_ACK && rxNum == sizeof(rxBuf.FormatAckReply))
            { //----------- ����� ACK_REPLY -----------
                // ���� ����� �� Id � ������� ��������
                uint8_t idOfTxBuf = FindTransactionIdInArray(rxBuf.FormatAckReply.Id, txContexts, SETTING__NUM_PACKETS_MAX);
                if (idOfTxBuf < SETTING__NUM_PACKETS_MAX)
                { // ������ ������ ��������� ������, ��� �������� ������ �������������
                    if (states[idOfTxBuf] == ID_EXCHANGE_STATE__WAIT_ACK_REPLY)
                    {
                        states[idOfTxBuf] = ID_EXCHANGE_STATE__NONE; // ���������� ������� ���������, ����� ����� ����������
                        // TODO: ��� �����?                    configASSERT(numPacketsInProcessing-- != 0);
                    }
                    else { /* ���� ���������� �������� ����� */ }
                }
                else { /* ���� ���������� �������� ����� */ }
            }
            else if (rxBuf.FormatCmdRead.Cmd == CMD_READ && rxNum == sizeof(rxBuf.FormatCmdRead))
            { //----------- ����� READ -----------
                //------------------------
                // �������� ACK_REQ
                //------------------------
                txContextAckReq.Packet.FormatAckRequest.Id = rxBuf.FormatAckReply.Id;
                // ��������� ���� ��������� ���� ���������������� � �� ��������
                StartToTransmit( &txContextAckReq );
                //------------------------
                // ���� ����� �� Id � ������� ��������
                uint8_t idOfTxBuf = FindTransactionIdInArray(rxBuf.FormatAckReply.Id, txContexts, SETTING__NUM_PACKETS_MAX);
                if (idOfTxBuf < SETTING__NUM_PACKETS_MAX)
                { // ������ ������ ��������� ������, � ������� ��� ����������� �����: ������, ��������� ����� (������� ����� ���� ��������� � ����� ������). 
                    states[idOfTxBuf] = ID_EXCHANGE_STATE__NEED_TX_DATA_FIRST;
                }
                else
                { // ������, ����� �������� �����
                    // ����� ���������� ��������� ������ ��� ������������ ������
                    uint8_t idOfTxBufToConstruct = FindExchangeStateInArray(ID_EXCHANGE_STATE__NONE, states, SETTING__NUM_PACKETS_MAX);
                    if (idOfTxBufToConstruct < SETTING__NUM_PACKETS_MAX)
                    { // ������ ������ ���������� ��������� ������
                        // ���������� ������� � ������ ��������� ������
                        uint32_t regReadVal = reg_read(FreeRTOS_ntohl(rxBuf.FormatCmdRead.RegAddr));
                        txContexts[idOfTxBufToConstruct].Packet.FormatCmdAnswer.Id = rxBuf.FormatCmdRead.Id;
                        txContexts[idOfTxBufToConstruct].Packet.FormatCmdAnswer.Cmd = rxBuf.FormatCmdRead.Cmd;
                        txContexts[idOfTxBufToConstruct].Packet.FormatCmdAnswer.RDataOrWStatus = FreeRTOS_htonl(regReadVal);
                        txContexts[idOfTxBufToConstruct].SizeOfPacket = sizeof(txContexts[idOfTxBufToConstruct].Packet.FormatCmdAnswer);

                        states[idOfTxBufToConstruct] = ID_EXCHANGE_STATE__NEED_TX_DATA_FIRST;
                    }
                    else { /* ���� ���������� �������� ����� */ }
                }
            }
            else if (rxBuf.FormatCmdWrite.Cmd == CMD_WRITE && rxNum == sizeof(rxBuf.FormatCmdWrite))
            { //----------- ����� WRITE -----------
                //------------------------
                // �������� ACK_REQ
                //------------------------
                txContextAckReq.Packet.FormatAckRequest.Id = rxBuf.FormatAckReply.Id;
                // ��������� ���� ��������� ���� ���������������� � �� ��������
                StartToTransmit(&txContextAckReq);
                //------------------------
                // ���� ����� �� Id � ������� ��������
                uint8_t idOfTxBuf = FindTransactionIdInArray(rxBuf.FormatAckReply.Id, txContexts, SETTING__NUM_PACKETS_MAX);
                if (idOfTxBuf < SETTING__NUM_PACKETS_MAX)
                { // ������, ��������� �����. ������ ������ ��������� ������, � ������� ��� ����������� �����
                    states[idOfTxBuf] = ID_EXCHANGE_STATE__NEED_TX_DATA_FIRST;
                }
                else
                { // ������, ����� �������� �����
                    // ����� ���������� ��������� ������ ��� ������������ ������
                    uint8_t idOfTxBufToConstruct = FindExchangeStateInArray(ID_EXCHANGE_STATE__NONE, states, SETTING__NUM_PACKETS_MAX);
                    if (idOfTxBufToConstruct < SETTING__NUM_PACKETS_MAX)
                    { // ������ ������
                        // ���������� ������� � ������ ��������� ������
                        uint32_t regWriteStatus = reg_write(FreeRTOS_ntohl(rxBuf.FormatCmdWrite.RegAddr, rxBuf.FormatCmdWrite.ValueToWrite));
                        txContexts[idOfTxBufToConstruct].Packet.FormatCmdAnswer.Id = rxBuf.FormatCmdWrite.Id;
                        txContexts[idOfTxBufToConstruct].Packet.FormatCmdAnswer.Cmd = rxBuf.FormatCmdWrite.Cmd;
                        txContexts[idOfTxBufToConstruct].Packet.FormatCmdAnswer.RDataOrWStatus = FreeRTOS_htonl(regWriteStatus);
                        txContexts[idOfTxBufToConstruct].SizeOfPacket = sizeof(txContexts[idOfTxBufToConstruct].Packet.FormatCmdAnswer);

                        states[idOfTxBufToConstruct] = ID_EXCHANGE_STATE__NEED_TX_DATA_FIRST;
                    }
                    else { /* ���� ���������� �������� ����� */ }
                }
            }
            else { /* ������� ��� ����������� ������� ���� �� ����� */ }
        }
        else { /* ��� ������ ����� ������ �� ������ */ }

        /****************************************/
        // �������� ��������� ����-���� �������� ��� ������� c �����. ��������

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
                        /* ���� ���������� ��������� ����-��� �������� ACK_REPLY, �� ����������� �������� ����� */
                        states[i] = ID_EXCHANGE_STATE__NONE;
                        // TODO: ��� �����?                    configASSERT(numPacketsInProcessing-- != 0);
                    }
                }
            }
        }

        /****************************************/
        /* �������� ������ ������ ��� ������� � �����. �������� */

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
            txContext.SizeOfPacket /*TODO: ������ ������ ���������� � �������*/,
            0 /* ulFlags with the FREERTOS_ZERO_COPY bit clear. */,
            ((struct TagParamsOfUDPConnectionTask*)pvParameters)->PtrDestinationAddress,
            sizeof(struct freertos_sockaddr)
        );
    }
}
//==============================================================================
