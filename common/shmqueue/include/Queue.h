
/***********************************************************
 * Author: Wen Li
 * Date  : 2/07/2022
 * Describe: Queue.h - FIFO Queue
 * History:
   <1> 7/24/2020 , create
************************************************************/
#ifndef _QUEUE_H_
#define _QUEUE_H_
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>


#define  BUF_SIZE               (64)
#define  SHM_QUEUE_CAP          ("SHM_QUEUE_CAP")
#define  SHM_QUEUE_KEY          ("SHM_QUEUE_KEY")


typedef enum
{
    MEMMOD_HEAP  = 1,
    MEMMOD_SHARE = 2
}MEMMOD;

typedef struct _QNode_
{
    unsigned TrcKey;
    unsigned IsReady;          
    char  Buf [BUF_SIZE];
}QNode;

static inline QNode* QBUF2QNODE (char *Qbuf)
{
    return (QNode*)(Qbuf - (sizeof (QNode)-BUF_SIZE));
}

void InitQueue (unsigned QueueNum, char *ShareMemKey, MEMMOD MemMode);
QNode* InQueue (void);
QNode* FrontQueue (void);
void OutQueue (void);
unsigned QueueSize (void);

void SetQueueExit ();
unsigned GetQueueExit ();
void DelQueue (void);



#endif 
