
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

/*******************************************************************************
* ��������� �������� ���� "�������" ������������ � ������������� ������.
*******************************************************************************/

#define CMD_READ   ((uint8_t)  0)
#define CMD_WRITE  ((uint8_t)  1)
#define CMD_ACK    ((uint8_t)  2)

/*-----------------------------------------------------------*/
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
// ��������� ��������� ������ ���������
enum TagExchangeStatesIds
{
    ID_EXCHANGE_STATE__NONE               = 0,
    ID_EXCHANGE_STATE__NEED_TX,                     // �������� ����� ��� ����������� - ��������� ��������� �����
    ID_EXCHANGE_STATE__WAIT_ACK_REPLY,              // ������������ �������� ��������� ��������� � ���������� �������� ������ ACK_REPLY - ����� ����������� ����� ����-����
    ID_EXCHANGE_STATE__NEED_REPEAT_REPLY,           // ���� ����-��� �������� ������ ACK_REPLY � ���������� ��������� ������� �� �������, ����� ��������� �����: �������� ����� ��� ����������� - ��������� ��������� �����
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



//==============================================================================

void task1UDPConnection( void *pvParameters )
{
    QueueSendHandle = xQueueCreate(SETTING__NUM_PACKETS_MAX, sizeof(struct TagTxContext));

    union TagRxBuffer         rxBuf;
    
    //---------------------------------------
    // ������������� � �������� ��������
    //---------------------------------------

    struct TagTxContext       txItems[SETTING__NUM_PACKETS_MAX];
    struct TagTxContext       txItemAckReq;
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

    txItemAckReq.Packet.FormatAckRequest.AckRequest = CMD_ACK; // �������� ����� �������� �� �����

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
        // + ������ �� �������: �������� - (rxNum <= 0)
        // ------ ���� state == ID_EXCHANGE_STATE__NEED_REPEAT_REPLY, ���� ����� - ������� � ��������� �������� ������, 
        // ����� - ���� state == ID_EXCHANGE_STATE__WAIT_ACK_REPLY, ���� ����� - ������� � �������� ��������� ����-����
        // 
        // - ������� � ���������� ������� (����������� ����� �������, ���������� ���� �� ������������� ������ �������) - ������� ��� ������ ���� �� �����
        // 
        // + ����� � �������: �������� - (cmd == CMD_READ) || (cmd == CMD_WRITE)
        // ++ ��������: �������� - (� �������� ������ ���� ����� �� ID) - ������� ACK_REQ, ���������, ������� �������� ����� - ������� � ����. �����
        // ++ ��������: �������� - (�����) - ���������, ������� ACK_REQ, ���������, ������� �������� ����� - ������� � ����. �����
        // 
        // - REPLY_ACK � ���������� ������� ������������ ID - ����� ID �� ���������� � ����� ������ - ������� ��� ������ ���� �� �����
        // + REPLY_ACK - ���� � ���. ������ � ID state == ID_EXCHANGE_STATE__WAIT_ACK_REPLY, �� ���������� state = NONE
        // ----- ���� status == ID_EXCHANGE_STATE__WAIT_ACK_REPLY - �� ���������� ������ ID_EXCHANGE_STATE__NONE, ����� -  - ������� � ����. �����




        int32_t rxNum = FreeRTOS_recvfrom(((struct TagParamsOfUDPConnectionTask*)pvParameters)->ClientSocket,
            &rxBuf, sizeof(rxBuf) /* = ������������ ������ ��������� ������ */,
            0 /* ulFlags with the FREERTOS_ZERO_COPY bit clear. */,
            ((struct TagParamsOfUDPConnectionTask*)pvParameters)->PtrDestinationAddress,
            & ((struct TagParamsOfUDPConnectionTask*)pvParameters)->SourceAddressLength /* Not used but should be set as shown. */
        );
        if (rxNum > 0)
        { // ���� ������� ������� ������
            /* Check packet consistency */
            /* ��������� ������������ ���������� ���� � ���� ������� */
            //----------- ����� ACK_REPLY -----------
            if (rxBuf.FormatAckReply.Cmd == CMD_ACK && rxNum == sizeof(rxBuf.FormatAckReply))
            { // ����� ACK_REPLY
                // ���� ����� �� Id � ������� ��������
                uint8_t idOfTxBuf = FindTransactionIdInArray(rxBuf.FormatAckReply.Id, txItems, SETTING__NUM_PACKETS_MAX);
                if (idOfTxBuf < SETTING__NUM_PACKETS_MAX)
                { // ������ ������ ��������� ������, ��� �������� ������ �������������
                    if (states[idOfTxBuf] == ID_EXCHANGE_STATE__WAIT_ACK_REPLY)
                    {
                        states[idOfTxBuf] = ID_EXCHANGE_STATE__NONE; // ���������� ������� ���������, ����� ����� ����������
                        // TODO: ��� �����?                    configASSERT(numPacketsInProcessing-- != 0);
                    }
                    else
                    { /* ���� ���������� �������� ����� */
                    }
                }
                else
                { /* ���� ���������� �������� ����� */
                }
            }
            //----------- ����� READ -----------
            else if (rxBuf.FormatCmdRead.Cmd == CMD_READ && rxNum == sizeof(rxBuf.FormatCmdRead))
            {
                // ���� ����� �� Id � ������� ��������
                uint8_t idOfTxBuf = FindTransactionIdInArray(rxBuf.FormatAckReply.Id, txItems, SETTING__NUM_PACKETS_MAX);
                if (idOfTxBuf < SETTING__NUM_PACKETS_MAX)
                { // ������, ��������� �����. ������ ������ ��������� ������, � ������� ��� ����������� �����
                    //------------------------
                    // �������� ACK_REQ
                    txItemAckReq.Packet.FormatAckRequest.Id = rxBuf.FormatAckReply.Id;
                    while (pdTRUE != xQueueSendToBack(QueueSendHandle, &txItemAckReq, 1)) { ; }
                    //------------------------
                    states[idOfTxBuf] = ID_EXCHANGE_STATE__NEED_TX;
                }
                else
                { // ������, ����� �������� �����
                    // ����� ���������� ��������� ������ ��� ������������ ������
                    uint8_t idOfTxBufToConstruct = FindExchangeStateInArray(ID_EXCHANGE_STATE__NONE, states, SETTING__NUM_PACKETS_MAX);
                    if (idOfTxBufToConstruct < SETTING__NUM_PACKETS_MAX)
                    { // ������ ������
                        //------------------------
                        // �������� ACK_REQ
                        txItemAckReq.Packet.FormatAckRequest.Id = rxBuf.FormatCmdRead.Id;
                        while (pdTRUE != xQueueSendToBack(QueueSendHandle, &txItemAckReq, 1)) { ; }
                        //------------------------
                        // ���������� ������� � ������ ��������� ������
                        uint32_t regReadVal = reg_read(FreeRTOS_ntohl(rxBuf.FormatCmdRead.RegAddr));
                        txItems[idOfTxBufToConstruct].Packet.FormatCmdAnswer.Id = rxBuf.FormatCmdRead.Id;
                        txItems[idOfTxBufToConstruct].Packet.FormatCmdAnswer.Cmd = rxBuf.FormatCmdRead.Cmd;
                        txItems[idOfTxBufToConstruct].Packet.FormatCmdAnswer.RDataOrWStatus = FreeRTOS_htonl(regReadVal);
                        states[idOfTxBufToConstruct] = ID_EXCHANGE_STATE__NEED_TX;
                    }
                    else
                    { /* ���� ���������� �������� ����� */
                    }
                }
            }
            //----------- ����� WRITE -----------
            else if (rxBuf.FormatCmdWrite.Cmd == CMD_WRITE && rxNum == sizeof(rxBuf.FormatCmdWrite))
            {
                // ���� ����� �� Id � ������� ��������
                uint8_t idOfTxBuf = FindTransactionIdInArray(rxBuf.FormatAckReply.Id, txItems, SETTING__NUM_PACKETS_MAX);
                if (idOfTxBuf < SETTING__NUM_PACKETS_MAX)
                { // ������, ��������� �����. ������ ������ ��������� ������, � ������� ��� ����������� �����
                    //------------------------
                    // �������� ACK_REQ
                    txItemAckReq.Packet.FormatAckRequest.Id = rxBuf.FormatAckReply.Id;
                    while (pdTRUE != xQueueSendToBack(QueueSendHandle, &txItemAckReq, 1)) { ; }
                    //------------------------
                    states[idOfTxBuf] = ID_EXCHANGE_STATE__NEED_TX;
                }
                else
                { // ������, ����� �������� �����
                    // ����� ���������� ��������� ������ ��� ������������ ������
                    uint8_t idOfTxBufToConstruct = FindExchangeStateInArray(ID_EXCHANGE_STATE__NONE, states, SETTING__NUM_PACKETS_MAX);
                    if (idOfTxBufToConstruct < SETTING__NUM_PACKETS_MAX)
                    { // ������ ������
                        //------------------------
                        // �������� ACK_REQ
                        txItemAckReq.Packet.FormatAckRequest.Id = rxBuf.FormatCmdWrite.Id;
                        while (pdTRUE != xQueueSendToBack(QueueSendHandle, &txItemAckReq, 1)) { ; }
                        //------------------------
                        // ���������� ������� � ������ ��������� ������
                        uint32_t regWriteStatus = reg_write(FreeRTOS_ntohl(rxBuf.FormatCmdWrite.RegAddr, rxBuf.FormatCmdWrite.ValueToWrite));
                        txItems[idOfTxBufToConstruct].Packet.FormatCmdAnswer.Id = rxBuf.FormatCmdWrite.Id;
                        txItems[idOfTxBufToConstruct].Packet.FormatCmdAnswer.Cmd = rxBuf.FormatCmdWrite.Cmd;
                        txItems[idOfTxBufToConstruct].Packet.FormatCmdAnswer.RDataOrWStatus = FreeRTOS_htonl(regWriteStatus);
                        states[idOfTxBufToConstruct] = ID_EXCHANGE_STATE__NEED_TX;
                    }
                    else
                    { /* ���� ���������� �������� ����� */ }
                }
            }
            else
            { /* ������� ��� ����������� ������� ���� �� ����� */ }
        }
        else
        { /* ��� ������ ����� ������ �� ������ */ }

        /****************************************/
        // �������� ��������� ����-���� �������� ��� ������� �� �������� ID_EXCHANGE_STATE__WAIT_ACK_REPLY
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
                        /* ���� ���������� ��������� ����-��� �������� ACK_REPLY,
                         �� ������ ������ �� ...NONE (����������� �������� �����)
                        */
                        states[i] = ID_EXCHANGE_STATE__NONE;
                        // TODO: ��� �����?                    configASSERT(numPacketsInProcessing-- != 0);
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

        // TODO: ���������� ������ ��� ����������
        for (int8_t nRepeats = 10; nRepeats > 0; nRepeats--)
        {
            /*TODO: ������ ������ tx ���������� � �������*/
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
            txItem.SizeOfPacket /*TODO: ������ ������ ���������� � �������*/,
            0 /* ulFlags with the FREERTOS_ZERO_COPY bit clear. */,
            ((struct TagParamsOfUDPConnectionTask*)pvParameters)->PtrDestinationAddress,
            sizeof(struct freertos_sockaddr)
        );
    }
}
//==============================================================================
