
#ifndef MY_UDP_CLIENT_H
#define MY_UDP_CLIENT_H


struct TagParamsOfUDPConnectionTask
{
    Socket_t ClientSocket;
    struct freertos_sockaddr DestinationAddress;
};


void task1UDPConnection(void* pvParameters);
void task2UDPConnection(void* pvParameters);


#endif /* MY_UDP_CLIENT_H */
