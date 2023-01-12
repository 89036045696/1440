
/* Standard includes. */
#include <stdio.h>
#include <conio.h>

/* Kernel includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "semphr.h"

/* XMODEM includes. */
#include "xmodem-1.0/xmodem.h"

/* Priorities at which the tasks are created. */
#define mainQUEUE_RECEIVE_TASK_PRIORITY		( tskIDLE_PRIORITY + 2 )
#define	mainQUEUE_SEND_TASK_PRIORITY		( tskIDLE_PRIORITY + 1 )

/* The rate at which data is sent to the queue.  The times are converted from
milliseconds to ticks using the pdMS_TO_TICKS() macro. */
#define mainTASK_SEND_FREQUENCY_MS			pdMS_TO_TICKS( 200UL )
#define mainTIMER_SEND_FREQUENCY_MS			pdMS_TO_TICKS( 2000UL )

/* This demo allows for users to perform actions with the keyboard. */
#define mainNO_KEY_PRESS_VALUE              ( -1 )
#define mainRESET_Rx_KEY                 ( 'r' )
#define mainRESET_Tx_KEY                 ( 't' )

/*-----------------------------------------------------------*/
/* Двоичный семафор для запуска Rx */
xSemaphoreHandle xSemaphoreRxStart;

/* Двоичный семафор для запуска Rx */
xSemaphoreHandle xSemaphoreTxStart;

/*-----------------------------------------------------------*/
xTaskHandle* HandleTaskTx = NULL;
xTaskHandle* HandleTaskRx = NULL;

SERIAL_TYPE hSerTx;
SERIAL_TYPE hSerRx;

/*-----------------------------------------------------------*/

/*** SEE THE COMMENTS AT THE TOP OF THIS FILE ***/
void main_xmodem( void )
{
    xSemaphoreRxStart = xSemaphoreCreateBinary();
    configASSERT(xSemaphoreRxStart != NULL);
    xSemaphoreTxStart = xSemaphoreCreateBinary();
    configASSERT(xSemaphoreTxStart != NULL);

    /* Start the two tasks as described in the comments at the top of this
    file. */
    portBASE_TYPE status;
    status = xTaskCreate(taskReceive,			/* The function that implements the task. */
        "Rx", 							/* The text name assigned to the task - for debug only as it is not used by the kernel. */
        configMINIMAL_STACK_SIZE, 		/* The size of the stack to allocate to the task. */
        NULL, 							/* The parameter passed to the task - not used in this simple case. */
        mainQUEUE_RECEIVE_TASK_PRIORITY,/* The priority assigned to the task. */
        HandleTaskRx);							/* The task handle is not required, so NULL is passed. */
    configASSERT(status == pdTRUE);

    status = xTaskCreate(taskSend, "TX", configMINIMAL_STACK_SIZE, NULL, mainQUEUE_SEND_TASK_PRIORITY, HandleTaskTx);
    configASSERT(pdTRUE == status);

    /* Start the tasks running. */
    vTaskStartScheduler();

    /* If all is well, the scheduler will now be running, and the following
    line will never be reached.  If the following line does execute, then
    there was insufficient FreeRTOS heap memory available for the idle and/or
    timer tasks	to be created.  See the memory management section on the
    FreeRTOS web site for more details. */
    configASSERT(pdFALSE);
    for (;; );
}
/*-----------------------------------------------------------*/

static void taskSend( void *pvParameters )
{
    int result = 0;
	/* Prevent the compiler warning about the unused parameter. */
	( void ) pvParameters;

    for (;; )
    {
        xSemaphoreTake(xSemaphoreTxStart, portMAX_DELAY);

        result = XSend(hSerRx, "TestOut.txt");
    }
}
/*-----------------------------------------------------------*/

static void taskReceive( void *pvParameters )
{
    int result = 0;

	/* Prevent the compiler warning about the unused parameter. */
	( void ) pvParameters;

	for( ;; )
	{
        xSemaphoreTake(xSemaphoreRxStart, portMAX_DELAY);

        result = XReceive(hSerRx, "TestIn.txt");
	}
}
/*-----------------------------------------------------------*/

/* Called from prvKeyboardInterruptSimulatorTask(), which is defined in main.c. */
void vBlinkyKeyboardInterruptHandler(int xKeyPressed)
{
    /* Handle keyboard input. */
    switch (xKeyPressed)
    {
    case mainRESET_Rx_KEY:

        if (HandleTaskRx != NULL) // TODO: проверять, что семафор не захвачен
        {
            xSemaphoreGive(xSemaphoreRxStart);
        }

        break;

    case mainRESET_Tx_KEY:

        if (HandleTaskTx != NULL) // TODO: проверять, что семафор не захвачен
        {
            xSemaphoreGive(xSemaphoreTxStart);
        }

        break;

    default:
        break;
    }
}


