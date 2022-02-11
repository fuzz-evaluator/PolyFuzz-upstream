/***********************************************************
 * Author: Wen Li
 * Date  : 11/18/2021
 * Describe: pl_message.h - pl message communicate with Fuzzing server
 * History:
   <1> 11/18/2021, create
************************************************************/
#ifndef __PL_MESSAGE_H__
#define __PL_MESSAGE_H__

typedef enum
{
    PL_MSG_STARTUP=1,
    PL_MSG_SEED,
    PL_MSG_ITR_BEGIN,
    PL_MSG_ITR_END,
    PL_MSG_FZ_OVER
}MSG_TYPE;

typedef struct MsgHdr
{
    unsigned MsgType;
    unsigned MsgLen;    
}MsgHdr;


typedef struct Msg
{
}MsgHdr;


#endif

