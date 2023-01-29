
/* Standard includes. */
#include <stdint.h>
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
// ��������� ������� ��������� �������
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
// ��������� ��������� ������ ���������
enum TagExchangeStatesIds
{
    ID_EXCHANGE_STATE__NONE               = 0,
    ID_EXCHANGE_STATE__ACK_REQUEST,                 // ������������ �������� ��������� ACK_REQUEST
    ID_EXCHANGE_STATE__WAIT_ACK_REPLY,              // ������������ �������� ��������� ��������� � ���������� �������� ������ ACK_REPLY
    ID_EXCHANGE_STATE__NEED_REPEAT_REPLY,           // ���� ����-��� �������� ������ ACK_REPLY � ���������� ��������� ������� �� �������: ����� ��������� �����
    ID_EXCHANGE_STATE__RECEIVED_REPEAT              // ������� ��������� � ��������� �������� (�.�. ������� �� ������ �������� ��������� �� ����� � �������� ����� ���� ��� ����������� - ��������� ACk � ��������� �����)
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
    // ������������� � �������� ��������
    //---------------------------------------

    struct TagTxContext       txItems[SETTING__NUM_PACKETS_MAX];
    enum TagExchangeStatesIds states[SETTING__NUM_PACKETS_MAX];
    TickType_t                momentsOfTxStartLast[SETTING__NUM_PACKETS_MAX]; // (ticks), ��� �������� �������� ����������� ����� ��������, � �� � ������ ��������.
    uint8_t                   numsOfFailAckReply[SETTING__NUM_PACKETS_MAX];
    //---------------------------------------
    uint8_t                   numPacketsInProcessing; // ����. ���������� ����������� � ��������� ���������� �������� �������
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
        /* ���� ���������� ��������� ����-��� �������� ACK_REPLY, �� ������ ������ �� ...NONE (����������� �������� �����)
        ����� ������� ID_EXCHANGE_STATE__WAIT_ACK_REPLY ��� ��������� � ����. ��������� ��������� �������� */
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
            /* ��������� ������������ ���������� ���� � ���� �������
            */
            if (rxBuf.FormatCmdRead.Cmd == CMD_READ && rxNum == sizeof(rxBuf.FormatCmdRead)) { ; /* ok */ }
            else if (rxBuf.FormatCmdWrite.Cmd == CMD_WRITE && rxNum == sizeof(rxBuf.FormatCmdWrite)) { ; /* ok */ }
            else
            {
                continue; // ���������� � ���������� ���������, �.�. ������ ��������� �� ���� ����� ���� �� ������.
            }
			/* ����� ������������� ������ ��������� ������:
			����� ���������� ��������� ������ (�������� - ������):
            */
			uint8_t idOfFreeTxBuffer = 0; // ���� >= SETTING__NUM_PACKETS_MAX, �� ��� ���������� ������
			for (  ; idOfFreeTxBuffer < SETTING__NUM_PACKETS_MAX; ++idOfFreeTxBuffer)
			{
                if ( states[idOfFreeTxBuffer] == ID_EXCHANGE_STATE__NONE )
                {
					break; // � idOfFreeTxBuffer ���������� ������
                }
            }
            /* Send REQ_ACK for "cmd" packets */
            txBuf.FormatAckRequest.Id = rxBuf.FormatCmdRead.Id;
            txBuf.FormatAckRequest.AckRequest = CMD_ACK;
            // TODO: 
             /*TODO: ������ ������ ���������� � �������*/
            while (pdTRUE != xQueueSendToBack(QueueSendHandle, &txItem, 1)) { ; }

            /* Parse cmd packet, execute command and build tx packet */
            uint32_t regReadValOrWriteStatus;
            switch (rxBuf.FormatCmdRead.Cmd)
            {
            case CMD_READ:
                regReadValOrWriteStatus = reg_read( FreeRTOS_ntohl(rxBuf.FormatCmdRead.RegAddr) );
                // ID ���������� ���������� � ������, ����� �������� ACK_REQ
                txBuf.FormatCmdAnswer.RDataOrWStatus = FreeRTOS_htonl(regReadValOrWriteStatus);
                break;

            case CMD_WRITE:
                regReadValOrWriteStatus = reg_write(FreeRTOS_ntohl(rxBuf.FormatCmdWrite.RegAddr),
                    FreeRTOS_ntohl(rxBuf.FormatCmdWrite.ValueToWrite));
                // ID ����������, ����� �������� ACK_REQ
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

        // TODO: ���������� ������ ��� ����������
        for (int8_t nRepeats = 10; nRepeats > 0; nRepeats--)
        {
            /*TODO: ������ ������ tx ���������� � �������*/
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

        /*TODO: ������ ������ tx ���������� � �������*/
        // ������ ������ ������������ ������ ������, � ������� �� ���� ������� � ���
        size_t sendLength = txBuf.bytes[1] == CMD_ACK ? sizeof(txBuf.FormatAckRequest) :
            /* CMD_READ or CMD_WRITE */ sizeof(txBuf.FormatCmdAnswer);
        FreeRTOS_sendto(((struct TagParamsOfUDPConnectionTask*)pvParameters)->ClientSocket,
            &txBuf,
            sendLength /*TODO: ������ ������ ���������� � �������*/,
            0 /* ulFlags with the FREERTOS_ZERO_COPY bit clear. */,
            ((struct TagParamsOfUDPConnectionTask*)pvParameters)->PtrDestinationAddress,
            sizeof(struct freertos_sockaddr)
        );
    }
}
//==============================================================================
