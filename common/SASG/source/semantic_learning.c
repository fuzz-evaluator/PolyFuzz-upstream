#include <pthread.h>
#include <dirent.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <math.h>
#include "db.h"
#include "Queue.h"
#include "ctrace/Event.h"
#include "pl_struct.h"

static PLServer g_plSrv;

extern char *get_current_dir_name(void);
BYTE* ReadFile (BYTE* SeedFile, DWORD *SeedLen, DWORD SeedAttr);



/////////////////////////////////////////////////////////////////////////////////////////////////////////
/// ple SERVER setup
/////////////////////////////////////////////////////////////////////////////////////////////////////////
static WORD pleSrvPort = 9999;
VOID SetSrvPort (WORD PortNo)
{
    pleSrvPort = PortNo;
    printf ("[SetSrvPort]set server port: %u \r\n", pleSrvPort);
}

static inline DWORD SrvInit (SocketInfo *SkInfo)
{
    SkInfo->SockFd = socket(AF_INET, SOCK_DGRAM, 0);
    if(SkInfo->SockFd < 0)
    {
        DEBUG ("Create socket fail....\r\n");
        return R_FAIL;
    }

    struct sockaddr_in addr_serv;
    int len;
    
    memset(&addr_serv, 0, sizeof(struct sockaddr_in));
    addr_serv.sin_family = AF_INET;
    addr_serv.sin_port   = htons(pleSrvPort);
    addr_serv.sin_addr.s_addr = htonl(INADDR_ANY);
    len = sizeof(addr_serv);
    
    if(bind(SkInfo->SockFd, (struct sockaddr *)&addr_serv, sizeof(addr_serv)) < 0)
    {
        DEBUG ("Bind socket to port[%d] fail....\r\n", SkInfo->SockFd);
        return R_FAIL;
    }

    char PortNo[32];
    snprintf (PortNo, sizeof (PortNo), "%u", pleSrvPort);
    setenv ("AFL_PL_SOCKET_PORT", PortNo, 1);
    return R_SUCCESS;
}


static inline BYTE* Recv (SocketInfo *SkInfo)
{
    memset (SkInfo->SrvRecvBuf, 0, sizeof(SkInfo->SrvRecvBuf));

    INT SkLen   = sizeof (struct sockaddr_in);
    INT RecvNum = recvfrom(SkInfo->SockFd, SkInfo->SrvRecvBuf, sizeof(SkInfo->SrvRecvBuf), 
                           0, (struct sockaddr *)&SkInfo->ClientAddr, (socklen_t *)&SkLen);
    assert (RecvNum != 0);

    return SkInfo->SrvRecvBuf;
}

static inline VOID Send (SocketInfo *SkInfo, BYTE* Data, DWORD DataLen)
{
    INT SkLen   = sizeof (struct sockaddr_in);
    INT SendNum = sendto(SkInfo->SockFd, Data, DataLen, 0, (struct sockaddr *)&SkInfo->ClientAddr, SkLen);
    assert (SendNum != 0);

    return;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////
/// Thread for ALF++ fuzzing process
/////////////////////////////////////////////////////////////////////////////////////////////////////////
void* FuzzingProc (void *Para)
{
    BYTE* DriverDir = (BYTE *)Para;    
    BYTE Cmd[1024];

    snprintf (Cmd, sizeof (Cmd), "cd %s; ./run-fuzzer.sh -P 2", DriverDir);
    printf ("CMD: %s \r\n", Cmd);
    int Ret = system (Cmd);
    assert (Ret >= 0);
    
    return NULL;
}


/////////////////////////////////////////////////////////////////////////////////////////////////////////
/// Database management
/////////////////////////////////////////////////////////////////////////////////////////////////////////
VOID InitDbTable (PLServer *plSrv)
{
    DWORD Ret;
    DbHandle *DHL = &plSrv->DHL;

    DHL->DBSeedHandle       = DB_TYPE_SEED;
    DHL->DBSeedBlockHandle  = DB_TYPE_SEED_BLOCK;
    DHL->DBBrVariableHandle = DB_TYPE_BR_VARIABLE;
    DHL->DBBrVarKeyHandle   = DB_TYPE_BR_VARIABLE_KEY;

    DHL->DBCacheBrVarKeyHandle = DB_TYPE_BR_VARIABLE_KEY_CACHE;
    DHL->DBCacheBrVariableHandle =  DB_TYPE_BR_VAR_CHACHE;

    InitDb(NULL);
    
    Ret = DbCreateTable(DHL->DBSeedHandle, 8*1024, sizeof (Seed), sizeof (DWORD));
    assert (Ret != R_FAIL);

    Ret = DbCreateTable(DHL->DBSeedBlockHandle, 64*1024, sizeof (SeedBlock), 96);
    assert (Ret != R_FAIL);

    Ret = DbCreateTable(DHL->DBBrVariableHandle, 128*1024, sizeof (BrVariable), 96);
    assert (Ret != R_FAIL);

    Ret = DbCreateTable(DHL->DBBrVarKeyHandle, 128*1024, sizeof (DWORD), sizeof (DWORD));
    assert (Ret != R_FAIL);

    Ret = DbCreateTable(DHL->DBCacheBrVarKeyHandle, 128*1024, sizeof (DWORD), sizeof (DWORD));
    assert (Ret != R_FAIL);
    
    Ret = DbCreateTable(DHL->DBCacheBrVariableHandle, 8*1024, sizeof (BrVariable), 96);
    assert (Ret != R_FAIL);

    printf ("@InitDB: SeedTable[%u], SeedBlockTable[%u], BrVarTable[%u], BrVarKeyTable[%u], CacheBrVarKeyTable[%u], CacheBrVarTable[%u]\r\n",
            TableSize (DHL->DBSeedHandle), TableSize (DHL->DBSeedBlockHandle),
            TableSize (DHL->DBBrVariableHandle), TableSize (DHL->DBBrVarKeyHandle), 
            TableSize (DHL->DBCacheBrVarKeyHandle), TableSize (DHL->DBCacheBrVariableHandle));
    return;
}


static inline BYTE* GetSeedName (BYTE *SeedPath)
{
    /* id:000005 (,src:000001), time:0,orig:test-2 */
    static BYTE SdName[FZ_SEED_NAME_LEN];
    memset (SdName, 0, sizeof (SdName));

    BYTE* ID = strstr (SeedPath, "id:");
    if (ID == NULL)
    {
        ID = strstr (SeedPath, "id-");
    }
    assert (ID != NULL);
    strncpy (SdName, ID, 9);
    SdName[2] = '-';
    SdName[9] = '-';

    BYTE* ORG = strstr (SeedPath, "orig");
    if (ORG == NULL)
    {
        ORG = strstr (SeedPath, "src");
        assert (ORG != NULL);
        strncpy (SdName+10, ORG, 10);
        SdName[13] = '-';
    }
    else
    {
        strcat (SdName, ORG);
        SdName[14] = '-';
    }

    return SdName;
}

static inline Seed* GetSeedByKey (DWORD SeedKey)
{
    DbReq Req;
    DbAck Ack;
    DWORD SeedId = SeedKey;

    Req.dwDataType = DB_TYPE_SEED;
    Req.dwKeyLen   = sizeof (DWORD);
    Req.pKeyCtx    = (BYTE*)&SeedId;

    DWORD Ret = QueryDataByKey(&Req, &Ack);
    if (Ret == R_SUCCESS)
    {
        return (Seed*)(Ack.pDataAddr);
    }
    else
    {
        return NULL;
    }
}

static inline Seed* AddSeed (BYTE* SeedName, DWORD SeedKey)
{    
    DbReq Req;
    DbAck Ack;
    DWORD SeedId = SeedKey;

    Req.dwDataType = DB_TYPE_SEED;
    Req.dwKeyLen   = sizeof (DWORD);
    Req.pKeyCtx    = (BYTE*)&SeedId;

    DWORD Ret = QueryDataByKey(&Req, &Ack);
    if (Ret == R_SUCCESS)
    {
        return (Seed*)(Ack.pDataAddr);
    }
    
    Ret = CreateDataByKey (&Req, &Ack);
    assert (Ret == R_SUCCESS);

    Seed* Sn = (Seed*)(Ack.pDataAddr);
    strncpy (Sn->SName, SeedName, sizeof (Sn->SName));
    Sn->LearnStatus = LS_NONE;
    Sn->BrVarChg    = 0;
    Sn->SeedKey     = SeedKey;

    return Sn;
}


static inline SeedBlock* AddSeedBlock (PilotData *PD, Seed* CurSeed, MsgIB *MsgItr)
{    
    DbReq Req;
    DbAck Ack;
    BYTE SKey [FZ_SEED_NAME_LEN+32] = {0};

    BYTE* SdName = GetSeedName (CurSeed->SName);

    Req.dwDataType = PD->DHL->DBSeedBlockHandle;
    Req.dwKeyLen = snprintf (SKey, sizeof (SKey), "%s-%u-%u", SdName, MsgItr->SIndex, MsgItr->Length);
    Req.pKeyCtx  = SKey;

    DWORD Ret = QueryDataByKey(&Req, &Ack);
    if (Ret == R_SUCCESS)
    {
        return (SeedBlock*)(Ack.pDataAddr);
    }
    
    Ret = CreateDataByKey (&Req, &Ack);
    assert (Ret == R_SUCCESS);

    SeedBlock* SBlk = (SeedBlock*)(Ack.pDataAddr);
    SBlk->Sd = CurSeed;

    ListInsert(&CurSeed->SdBlkList, SBlk);

    DEBUG ("@@@ AddSeedBlock: %s \r\n", SKey);

    return SBlk;
}


static inline DWORD AddBrVarKey (DWORD DataType, DWORD Key)
{    
    DbReq Req;
    DbAck Ack;
    DWORD Ret;

    Req.dwDataType = DataType;
    Req.dwKeyLen   = sizeof (DWORD);
    Req.pKeyCtx    = (BYTE*)&Key;

    Ret = QueryDataByKey(&Req, &Ack);
    if (Ret != R_SUCCESS)
    {
        Ret = CreateDataByKey (&Req, &Ack);
        assert (Ret == R_SUCCESS);

        DWORD *BrValKey = (DWORD*)(Ack.pDataAddr);
        *BrValKey = Key;

        return TRUE;
    }
    else
    {
        return FALSE;
    }
}


static inline VOID SetBrVFalg (BrVariable *BrVal, DWORD Bit, DWORD Value)
{
    DWORD No = Bit>>3;
    DWORD Offset = Bit&0x7;

    if (Value)
    {
        BrVal->ValideTag[No] |= (1<<Offset);
    }
    else
    {
        BrVal->ValideTag[No] &= ~(1<<Offset);
    }

    return;
}


static inline BOOL CheckBrVFalg (BrVariable *BrVal, DWORD Bit)
{
    DWORD No = Bit>>3;
    DWORD Offset = Bit&0x7;

    if (No < FZ_SAMPLE_BITNUM)
    {
        return (BrVal->ValideTag[No] & (1<<Offset));
    }
    else
    {
        return FALSE;
    }    
}

static inline VOID CacheBrVar (PilotData *PD, DWORD Key, ObjValue *Ov, DWORD QItr)
{    
    DbReq Req;
    DbAck Ack;
    DWORD Ret;
    BYTE SKey [FZ_SEED_NAME_LEN+32] = {0};

    if (QItr >= PD->PLOP->SampleNum)
    {
        return;
    }

    SeedBlock* SBlk = PD->CurSdBlk;
    BYTE* SdName = GetSeedName (SBlk->Sd->SName);

    Req.dwDataType = PD->DHL->DBCacheBrVariableHandle;
    Req.dwKeyLen   = snprintf (SKey, sizeof (SKey), "%s-%u-%u-%x", SdName, SBlk->SIndex, SBlk->Length, Key);
    Req.pKeyCtx    = SKey;

    Ret = QueryDataByKey(&Req, &Ack);
    if (Ret != R_SUCCESS)
    {
        Ret = CreateDataByKey (&Req, &Ack);
        assert (Ret == R_SUCCESS);
    }

    BrVariable *BrVal = (BrVariable*)(Ack.pDataAddr);

    while (BrVal->ValIndex < QItr)
    {
        SetBrVFalg (BrVal, BrVal->ValIndex, FALSE);
        BrVal->ValIndex++;
    }

    /* In current implementation, we only save one value in an iteration */
    if (BrVal->ValIndex > QItr)
    {
        BrVal->ValIndex = QItr;
    }
    
    BrVal->Key  = Key;
    BrVal->Type = Ov->Type;
    BrVal->Value [BrVal->ValIndex] = Ov->Value;
    SetBrVFalg (BrVal, BrVal->ValIndex, TRUE);
    BrVal->ValIndex++;
    
    BrVal->ValNum++;

    DEBUG ("CacheBrVar ->[%u/%u][%u]Key:%s, Value:%u\r\n", (DWORD)BrVal->ValNum, (DWORD)BrVal->ValIndex, Key, SKey, (DWORD)Ov->Value);

    return;
}


static inline BYTE* GetDataByID (DWORD DataType, DWORD DataID)
{
    DbReq QueryReq;
    DbAck QueryAck;

    QueryReq.dwDataType = DataType;
    QueryReq.dwDataId   = DataID;
    DWORD Ret = QueryDataByID(&QueryReq, &QueryAck);
    assert (Ret != R_FAIL);

    return QueryAck.pDataAddr;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////
/// Thread for dynamic event collection
/////////////////////////////////////////////////////////////////////////////////////////////////////////
static inline VOID IncLearnStat (PilotData *PD, DWORD BrNum)
{
    SeedBlock* SBlk = PD->CurSdBlk;

    DWORD LearnIndex = SBlk->SIndex/LEARN_BLOCK_SIZE;
    if (LearnIndex < LEARN_BLOCK_NUM)
    {
        PD->LearnStat[LearnIndex] += BrNum;
    }
    else
    {
        PD->LearnStat[LEARN_BLOCK_NUM-1] += BrNum;
    } 
}


static inline BOOL IsInLearnStat (PilotData *PD, DWORD OFF)
{
    if (PD->LsValidNum == 0)
    {
        return TRUE;
    }
    
    DWORD LearnIndex = OFF/LEARN_BLOCK_SIZE;
    if (LearnIndex >= LEARN_BLOCK_NUM)
    {
        LearnIndex = LEARN_BLOCK_NUM-1;
    }

    if (PD->LearnStat [LearnIndex] == 0)
    {
        return FALSE;
    }
    else
    {
        return TRUE;
    }
}



static inline VOID LoadBlockStat (PilotData *PD)
{
    FILE *pf = fopen (BLOCK_STAT, "r");
    if (pf == NULL)
    {
        memset (PD->LearnStat, 0, sizeof (PD->LearnStat));
        PD->LsValidNum = 0;
        return;
    }

    BYTE ReadBuf[2048];
    char *Buf = fgets (ReadBuf, sizeof (ReadBuf), pf);
    assert (Buf != NULL);

    printf ("******************************  LoadBlockStat  ******************************\r\n");
    PD->LsValidNum = 0;
    DWORD Index = 0;
    Buf = strtok (ReadBuf, ",");
    while(Buf != NULL) 
    {
        DWORD BrStat = (DWORD)atoi (Buf);
        if (BrStat != 0)
        {
            DWORD Start = Index * LEARN_BLOCK_SIZE;
            DWORD End   = Start + LEARN_BLOCK_SIZE;
            printf ("\t[%-2u -> %-2u]: %u \r\n", Start, End, BrStat);
            PD->LearnStat [Index] = 1;
            PD->LsValidNum++;
        }
        
        Buf = strtok(NULL, ",");
        Index++;
    }
    printf ("*****************************************************************************\r\n");
    return;
}


static inline VOID DumpBlockStat (PilotData *PD)
{
    FILE *pf = fopen (BLOCK_STAT, "w");
    assert (pf != NULL);
    
    for (DWORD ix = 0; ix < LEARN_BLOCK_NUM; ix++)
    {
        fprintf (pf, "%u,", PD->LearnStat [ix]);
    }
    fclose (pf);
    
    return;
}

static inline VOID GetLearnStat (PilotData *PD)
{
    printf ("******************************  ShowLearnStat  ******************************\r\n");
    PD->LsValidNum = 0;
    for (DWORD ix = 0; ix < LEARN_BLOCK_NUM; ix++)
    {
        DWORD Stat  = PD->LearnStat [ix];
        if (Stat == 0)
        {
            continue;
        }
        
        DWORD Start = ix * LEARN_BLOCK_SIZE;
        DWORD End   = Start + LEARN_BLOCK_SIZE;
        if (ix + 1 < LEARN_BLOCK_NUM)
        {
            printf ("\t[%-2u -> %-2u]: %u \r\n", Start, End, Stat);
        }
        else
        {
            printf ("\t[%-2u -> +++]: %u \r\n", Start, Stat);
        }
        PD->LsValidNum++;
    }
    printf ("*****************************************************************************\r\n");

    DumpBlockStat (PD);
    return;
}


void* DECollect (void *Para)
{
    PilotData *PD  = (PilotData*)Para;
    DbHandle *DHL  = PD->DHL;
    DWORD PreAlgin = 0;

    while (PD->PilotStatus == PILOT_ST_CLOOECTING)
    {
        sem_wait(&PD->CollectStartSem);
        
        DWORD QSize = QueueSize ();
        DEBUG ("[DECollect] entry with QUEUE size: %u.....\r\n", QSize);
        
        DWORD QItr  = 0;
        DWORD BrKeyNum = 0;
        DWORD ProccEvent = 0;
        while (PD->FzExit == FALSE || QSize != 0)
        {
            QNode *QN = FrontQueue ();
            if (QN == NULL || QN->IsReady == FALSE)
            {
                if (QN && (time (NULL) - QN->TimeStamp >= 1))
                {
                    OutQueue (QN);
                }
                QSize = QueueSize ();  
                continue;
            }

            if (QN->TrcKey == TARGET_EXIT_KEY)
            {
                QItr ++;
                if (QItr > PD->PLOP->SampleNum)
                {
                    /* this is an abnormal, but we can not break directly 
                       to make sure the queue be emptied*/
                    ;
                }
                DEBUG ("##### [%u][QSize-%u]QUEUE: KEY:%x, Where:%u, target exit.... \r\n", 
                        QItr, QSize, QN->TrcKey, ((ExitInfo*)(QN->Buf))->Where);
            }
            else
            {
                ObjValue *OV = (ObjValue *)QN->Buf;

                BrKeyNum += AddBrVarKey (PD->DHL->DBCacheBrVarKeyHandle, QN->TrcKey);

                CacheBrVar (PD, QN->TrcKey, OV, QItr);
                DEBUG ("[%u][QSize-%u]QUEUE: KEY:%u - [type:%u, length:%u]Value:%lu \r\n", 
                        QItr , QSize, QN->TrcKey, (DWORD)OV->Type, (DWORD)OV->Length, OV->Value);
            }

            ProccEvent++;
            OutQueue (QN);
            QSize = QueueSize ();
            
        }   
        DEBUG ("DECollect loop over.....[%u] EVENT processed\r\n", ProccEvent);

        SeedBlock* SBlk = PD->CurSdBlk;
        if (BrKeyNum == 0 && SBlk->Length == PreAlgin)
        {       
            DEBUG ("\t@@@DECollect --- [%s][%u-%u]No new branch varables captured....\r\n", 
                   GetSeedName (SBlk->Sd->SName),
                   SBlk->SIndex, SBlk->Length);
            ResetTable (DHL->DBCacheBrVariableHandle);
        }
        else
        {
            CopyTable (DHL->DBBrVariableHandle, DHL->DBCacheBrVariableHandle);
            printf ("\t@@@DECollect --- New BR found [to %u]. [Table%u] DataNum = %u after copy [Table%u][%u]! ---> Reset CacheBr to ",
                    QueryDataNum(DHL->DBCacheBrVarKeyHandle),
                    DHL->DBBrVariableHandle, QueryDataNum(DHL->DBBrVariableHandle),
                    DHL->DBCacheBrVariableHandle, QueryDataNum(DHL->DBCacheBrVariableHandle));
            
            ResetTable (DHL->DBCacheBrVariableHandle);
            printf ("[%u] \r\n", TableSize(DHL->DBCacheBrVariableHandle));

            IncLearnStat (PD, BrKeyNum);
        }
        PreAlgin = SBlk->Length;
        
        sem_post(&PD->CollectEndSem);
    }
    
    pthread_exit ((void*)0);
}


/////////////////////////////////////////////////////////////////////////////////////////////////////////
/// Main logic of PLE server
/////////////////////////////////////////////////////////////////////////////////////////////////////////
static inline pthread_t CollectBrVariables (PilotData *PD)
{
    pthread_t Tid = 0;
    int Ret = pthread_create(&Tid, NULL, DECollect, PD);
    if (Ret != 0)
    {
        fprintf (stderr, "pthread_create for DECollect fail, Ret = %d\r\n", Ret);
        exit (0);
    }

    return Tid;
}

static inline MsgHdr* FormatMsg (SocketInfo *SkInfo, DWORD MsgType)
{
    MsgHdr *MsgH;
    
    MsgH = (MsgHdr *)SkInfo->SrvSendBuf;
    MsgH->MsgType = MsgType;
    MsgH->MsgLen  = sizeof (MsgHdr);

    return MsgH;
}

static inline VOID MakeDir (BYTE *DirPath)
{
    struct stat st = {0};
    
    if (stat(DirPath, &st) == -1) 
    {
        mkdir(DirPath, 0777);
    }

    return;
}


static inline BOOL CheckVariant (BrVariable *BrVal, DWORD SampleNum)
{  
    if (BrVal->ValNum < 4)
    {
        return FALSE;
    }
    
    DWORD SameNum = 0;
    DWORD PreVal  = -1u;
    for (DWORD Index = 0; Index < SampleNum && Index < BrVal->ValNum; Index++)
    {
         ULONG CurVal = BrVal->Value[Index];
         if (PreVal == CurVal)
         {
            SameNum++;
         }

         PreVal = CurVal;
    }

    if (SameNum >= SampleNum/4)
    {
        return FALSE;
    }
    else
    {
        return TRUE;
    }
}

static inline BYTE* GenAnalysicData (PilotData *PD, BYTE *BlkDir, SeedBlock *SdBlk, DWORD VarKey)
{
    static BYTE VarFile[1048];
    DbReq Req;
    DbAck Ack;
    DWORD Ret;
    BYTE SKey [FZ_SEED_NAME_LEN] = {0};

    BYTE* SdName = GetSeedName (SdBlk->Sd->SName);

    Req.dwDataType = PD->DHL->DBBrVariableHandle;
    Req.dwKeyLen   = snprintf (SKey, sizeof (SKey), "%s-%u-%u-%x", SdName, SdBlk->SIndex, SdBlk->Length, VarKey);
    Req.pKeyCtx    = SKey;
    Ret = QueryDataByKey(&Req, &Ack);
    if (Ret != R_SUCCESS)
    {
        DEBUG ("Query fail -> [BrVariable]: %s \r\n", SKey);
        return NULL;
    }
    
    BrVariable *BrVal = (BrVariable*)Ack.pDataAddr;
    if (CheckVariant (BrVal, PD->PLOP->SampleNum) == FALSE)
    {
        DEBUG ("[CheckVariant] false...\r\n");
        return NULL;
    }

    MakeDir (BlkDir);

    /* FILE NAME */
    snprintf (VarFile, sizeof (VarFile), "%s/Var-%u.csv", BlkDir, VarKey);
    DEBUG ("[GenAnalysicData]%s \r\n", VarFile);
    FILE *F = fopen (VarFile, "w");
    assert (F != NULL);

    fprintf (F, "SDBLK-%u-%u,BrVar-%u\n", SdBlk->SIndex, SdBlk->Length, VarKey);
    for (DWORD Index = 0; Index < PD->PLOP->SampleNum; Index++)
    {
        if (CheckBrVFalg(BrVal, Index) == FALSE)
        {
            continue;
        }

        switch (BrVal->Type)
        {
            case VT_CHAR:
            {
                fprintf (F, "%u,%d\n", (DWORD)SdBlk->Value[Index], (CHAR)BrVal->Value[Index]);
                break;
            }
            case VT_WORD:
            {
                fprintf (F, "%u,%d\n", (DWORD)SdBlk->Value[Index], (SWORD)BrVal->Value[Index]);
                break;
            }
            case VT_DWORD:
            {
                fprintf (F, "%u,%d\n", (DWORD)SdBlk->Value[Index], (SDWORD)BrVal->Value[Index]);
                break;
            }
            case VT_LONG:
            {
                fprintf (F, "%u,%ld\n", (DWORD)SdBlk->Value[Index], (long)BrVal->Value[Index]);
                break;
            }
            default:
            {
                assert (0);
            }
        }  
    }
    fclose (F);

    return VarFile;    
}

static inline DWORD IsValueValid (ULONG Value, DWORD Align)
{
    switch (Align)
    {
        case 1:
        {
            if ((Value & 0xFFFFFF00) == 0)
            {
                return TRUE;
            }
            break;
        }
        case 2:
        {
            if ((Value & 0xFFFF0000) == 0)
            {
                return TRUE;
            }
            break;
        }
        case 4:
        {
            if ((Value & 0xFFFFFFFF00000000) == 0)
            {
                return TRUE;
            }
            break;
        }
        default:
        {
            return  TRUE;
        }
    }

    return FALSE;
}


int CmpBrValue (const void *a, const void *b) 
{
    int V1 = *(int *)a;
    int V2 = *(int *)b;
    return (V1 - V2);
}


static inline VOID ReadBsList (BYTE* BsDir, BsValue *BsList, DWORD Align)
{
    DIR *Dir;
    struct dirent *SD;
    BYTE BSfPath[1024];
    BYTE BSValStr[128];

    Dir = opendir((const char*)BsDir);
    if (Dir == NULL)
    {
        return;
    }

    List *FList = ListAllot();   
    while (SD = readdir(Dir))
    {
        if (strstr (SD->d_name, ".csv.bs") == NULL)
        {
            continue;
        }

        BYTE *Bsf = strdup (SD->d_name);
        ListInsert(FList, Bsf);
    }
    closedir (Dir);
    
    if (FList->NodeNum == 0)
    {
        return;
    }

    DWORD BaseNum = FList->NodeNum * 256;
    BsList->ValueList = (ULONG *)malloc (BaseNum * sizeof (ULONG));
    BsList->ValueCap  = BaseNum;
    BsList->ValueNum  = 0;
    BsList->VarNum    = FList->NodeNum;
    assert (BsList->ValueList != NULL);

    LNode *LN = FList->Header;
    while (LN != NULL)
    {
        BYTE* FName = (BYTE *)LN->Data;
        snprintf (BSfPath, sizeof (BSfPath), "%s/%s", BsDir, FName);
        DEBUG ("### Get BSF: %s \r\n", BSfPath);

        FILE *BSF = fopen (BSfPath, "r");
        assert (BSF != NULL);

        while (!feof (BSF))
        {
            if (fgets (BSValStr, sizeof (BSValStr), BSF) != NULL)
            {
                ULONG Val = strtol(BSValStr, NULL, 10);
                if (IsValueValid (Val, Align) == FALSE)
                {
                    continue;
                }

                DWORD i;
                for (i = 0; i < BsList->ValueNum; i++)
                {
                    if (BsList->ValueList[i] == Val)
                    {
                        break;
                    }
                }

                if (i < BsList->ValueNum)
                {
                    continue;
                }

                if (BsList->ValueNum >= BsList->ValueCap)
                {
                    DEBUG ("\t Realloc ValueList from %u to %u \r\n", BsList->ValueCap, BsList->ValueCap+BaseNum);
                    BsList->ValueCap  = BsList->ValueCap + BaseNum;
                    BsList->ValueList = (ULONG *)realloc (BsList->ValueList, BsList->ValueCap * sizeof (ULONG));
                    assert (BsList->ValueList != NULL);
                }

                BsList->ValueList[BsList->ValueNum] = Val;
                BsList->ValueNum++;
                DEBUG ("\t read value: %lu \r\n", Val);
            }
        }

        LN = LN->Nxt;
    }

    if (BsList->ValueNum != 0)
    {
        qsort (BsList->ValueList, BsList->ValueNum, sizeof(ULONG), CmpBrValue);
    }

    ListDel(FList, free);
    return;
}


static inline BYTE* GenSeedDir (DWORD CurAlign)
{
    static BYTE SeedDir[256];
    snprintf (SeedDir, sizeof (SeedDir), "%s/Align%u", GEN_SEED, CurAlign);
    MakeDir(SeedDir);
    return SeedDir;
}

static inline DWORD ReadGenSeedUnit (PilotData *PD)
{
    DWORD GenSeedUnit;

    mutex_lock (&PD->GenSeedLock);
    GenSeedUnit = PD->GenSeedNumUnit;
    mutex_unlock (&PD->GenSeedLock);

    return GenSeedUnit;
}

static inline VOID ResetGenSeedUnit (PilotData *PD)
{
    mutex_lock (&PD->GenSeedLock);
    PD->GenSeedNumUnit = 0;
    mutex_unlock (&PD->GenSeedLock);
    return;
}


static inline VOID SendSeedsForFuzzing ()
{
    PLServer *plSrv = &g_plSrv;
    PilotData *PD = &plSrv->PD;

    MsgHdr *MsgSend;
    unsigned MsgType = PL_MSG_GEN_SEED;
    if (PD->GenSeedNumBlock < GEN_SEED_MAXNUM)
    {
        MsgSend  = FormatMsg(&plSrv->SkInfo, MsgType);
        printf ("[SendSeedsForFuzzing] send PL_MSG_GEN_SEED[Align%u]:%u.\r\n", PD->CurAlign, PD->GenSeedNum/GEN_SEED_UNIT);
    }
    else
    {
        MsgType  = PL_MSG_GEN_SEED_DONE;
        MsgSend  = FormatMsg(&plSrv->SkInfo, MsgType);
        printf ("[SendSeedsForFuzzing] send PL_MSG_GEN_SEED_DONE[Align%u]:%u.\r\n", PD->CurAlign, PD->GenSeedNum/GEN_SEED_UNIT);
    }
    BYTE *GenSeedDir = (BYTE*)(MsgSend+1);
    MsgSend->MsgLen += sprintf (GenSeedDir, "%s/%s/Align%u", get_current_dir_name (), GEN_SEED, PD->CurAlign);
    Send(&plSrv->SkInfo, (BYTE*)MsgSend, MsgSend->MsgLen);

    MsgHdr *MsgRev = (MsgHdr *) Recv(&plSrv->SkInfo);
    assert (MsgRev->MsgType == MsgType);

    ResetGenSeedUnit (PD);
    PD->PilotStatus = PILOT_ST_LEARNING;

    return;    
}

static inline VOID WaitFuzzingOnFly (PilotData *PD, DWORD IsEnd)
{  
    DWORD GenSeedUnit = ReadGenSeedUnit (PD);
    if (IsEnd == FALSE)
    {
        if (GenSeedUnit == GEN_SEED_UNIT)
        {
            PD->PilotStatus = PILOT_ST_SEEDING;
            PD->GenSeedNum += GEN_SEED_UNIT;
            while (TRUE)
            {
                GenSeedUnit = ReadGenSeedUnit (PD);
                if (GenSeedUnit == 0)
                {
                    break;
                }
                
                sleep (1);
            }        
        }
    }
    else
    {
        if (PD->GenSeedNumUnit > 0)
        {
            PD->PilotStatus = PILOT_ST_SEEDING;
            PD->GenSeedNum += PD->GenSeedNumUnit;
            PD->GenSeedNumBlock += GEN_SEED_MAXNUM;
            while (TRUE)
            {
                GenSeedUnit = ReadGenSeedUnit (PD);
                if (GenSeedUnit == 0)
                {
                    break;
                }
                
                sleep (1);
            }  
        }
    }

    return;
}


static inline VOID WriteSeed (PilotData *PD, DWORD BlkNum, ULONG *CurSeed)
{
    BYTE *SeedDir = GenSeedDir (PD->CurAlign);
    snprintf (PD->NewSeedPath, sizeof (PD->NewSeedPath), "%s/%s_%u-%u", 
              SeedDir, PD->CurSeedName, PD->CurAlign, PD->GenSeedNum + PD->GenSeedNumUnit);
    FILE *SF = fopen (PD->NewSeedPath, "w");
    if (SF == NULL)
    {
        printf ("@@@ Warning: [Align%u]create seed file fail, GenSeedNum = %u\r\n", PD->CurAlign, PD->GenSeedNum);
        return;
    }
                
    for (DWORD si = 0; si < BlkNum; si++)
    {
        switch (PD->CurAlign)
        {
            case 1:
            {
                BYTE Val = (BYTE)CurSeed[si];
                fwrite (&Val, sizeof (Val), 1, SF);
                break;
            }
            case 2:
            {
                WORD Val = (WORD)CurSeed[si];
                fwrite (&Val, sizeof (Val), 1, SF);
                break;
            }
            case 4:
            {
                SDWORD Val = (SDWORD)CurSeed[si];
                fwrite (&Val, sizeof (Val), 1, SF);
                break;
            }
            case 8:
            {
                ULONG Val = (ULONG)CurSeed[si];
                fwrite (&Val, sizeof (Val), 1, SF);
                break;
            }
            default:
            {
                assert (0);
            }
        }
    }
    
    fclose (SF);    
    PD->GenSeedNumUnit++;
    PD->GenSeedNumBlock++;
    WaitFuzzingOnFly (PD, FALSE);

    return;
}


static inline DWORD GetSampleNum (PilotData *PD, BsValue *BsHeader)
{
    DWORD SampleNum = BsHeader->ValueNum;
    if (PD->PLOP->SamplePolicy == SP_AVERAGE)
    {
        if (PD->AvgSamplingNum != 0 && SampleNum > PD->AvgSamplingNum)
        {
            SampleNum = PD->AvgSamplingNum;
        }
    }
    else
    {
        assert (PD->PLOP->SamplePolicy == SP_VARNUM);
        SampleNum = BsHeader->VarWeight;
        if (SampleNum > BsHeader->ValueNum)
        {
            SampleNum = BsHeader->ValueNum;
        }
        SampleNum = (SampleNum == 0)?1:SampleNum;
    }

    return SampleNum;
}

static inline VOID GenSeed (PilotData *PD, BsValue *BsHeader, DWORD BlkNum, DWORD CurBlkNo, ULONG *CurSeed)
{
    if (PD->GenSeedNumBlock >= GEN_SEED_MAXNUM)
    {
        return;
    }

    DWORD SampleNum = GetSampleNum (PD, BsHeader);
    if (SampleNum == 1)
    {
        do
        {
            CurSeed[CurBlkNo] = BsHeader->ValueList[0];
            BsHeader++; CurBlkNo++;

            if (CurBlkNo == BlkNum)
            {
                WriteSeed (PD, BlkNum, CurSeed);
                PD->SdStatByBlock[PD->CurAlign]++;
                break;
            }
            
            SampleNum = GetSampleNum (PD, BsHeader); 
        } while (SampleNum == 1);

        if (CurBlkNo == BlkNum)
        {
            return;
        }
    }

    DWORD SpUnit = BsHeader->ValueNum/SampleNum;
    SpUnit = (SpUnit == 0)?1:SpUnit;

    //printf ("[GenSeed]CurBlkNo:%u, SpUnit = %u \r\n", CurBlkNo, SpUnit);
    for (DWORD Ei = 0; Ei < SampleNum; Ei++)
    {
        /* random sampling */
        DWORD SmpIndex = Ei*SpUnit +  random ()%SpUnit;            
        CurSeed[CurBlkNo] = BsHeader->ValueList[SmpIndex];
            
        if (CurBlkNo+1 == BlkNum)
        {
            WriteSeed (PD, BlkNum, CurSeed);
            PD->SdStatByBlock[PD->CurAlign]++;
        }
        else
        {      
            GenSeed (PD, BsHeader+1, BlkNum, CurBlkNo+1, CurSeed);
        }
    }
}


static inline VOID ComputeBudget (PilotData *PD, BsValue *SAList, DWORD SANum, DWORD ValidSANum)
{
    PD->AvgSamplingNum = 0;

    DWORD VarNum  = 0;
    ULONG SeedNum = 1;

    if (PD->PLOP->SamplePolicy == SP_AVERAGE)
    {
        for (DWORD ix = 0; ix < SANum; ix++)
        {
            assert (SAList[ix].ValueNum != 0);
            SeedNum *= SAList[ix].ValueNum;
            VarNum  += SAList[ix].VarNum;
        }
        
        if (SeedNum <= GEN_SEED_MAXNUM)
        {
            printf ("[ComputeBudget][Block:%u/%u]Total SeedNum = %lu, VarNum = %u\r\n", ValidSANum, SANum, SeedNum, VarNum);
            return;
        }
    
        DWORD AvgSample = (DWORD)(pow (GEN_SEED_MAXNUM, 1.0/ValidSANum) + 1);
        if (AvgSample <= 1) AvgSample = 2;

        PD->AvgSamplingNum = AvgSample;
        printf ("[ComputeBudget-SP_AVERAGE][Block:%u/%u]Theoretical SeedNum = %lu, AVG-sampling: %u, VarNum = %u\r\n", 
                ValidSANum, SANum, SeedNum, AvgSample, VarNum);
    }
    else
    {
        assert (PD->PLOP->SamplePolicy == SP_VARNUM);
        DWORD Diff = 0;
        ULONG SdCombo;
        do
        {
            SdCombo = 1;
            for (DWORD ix = 0; ix < SANum; ix++)
            {
                if (SAList[ix].VarNum != 0)
                {
                    SeedNum *= (SAList[ix].VarNum + Diff);
                }
                SdCombo *= SAList[ix].ValueNum;
            }

            if (SdCombo < GEN_SEED_MAXNUM || SeedNum >= GEN_SEED_MAXNUM)
            {
                break;
            }
            
            Diff++;
        }while (TRUE);

        SeedNum = 1;
        for (DWORD ix = 0; ix < SANum; ix++)
        {
            if (SAList[ix].VarNum != 0)
            {
                if (SdCombo < GEN_SEED_MAXNUM)
                {
                    SAList[ix].VarWeight = SAList[ix].ValueNum;
                }
                else
                {
                    SAList[ix].VarWeight = SAList[ix].VarNum + Diff;
                }

                SeedNum *= SAList[ix].VarWeight;
            }
            VarNum  += SAList[ix].VarNum;
        }
        printf ("[ComputeBudget-SP_VARNUM][Block:%u/%u]SeedNum = %lu, VarNum = %u, Diff = %u\r\n", ValidSANum, SANum, SeedNum, VarNum, Diff);
    }

    return;    
}


static inline VOID DumpSdStat (PilotData *PD)
{
    FILE *f = fopen ("seed_stat_blocksize.txt", "a+");
    if (f == NULL)
    {
        return;
    }

    fprintf (f, "1:%u, 2:%u, 4:%u, 8:%u, 16:%u\r\n",
             PD->SdStatByBlock[1], PD->SdStatByBlock[2],
             PD->SdStatByBlock[4], PD->SdStatByBlock[8],
             PD->SdStatByBlock[16]);
    fclose (f);

    return;
}

static inline VOID GenAllSeeds (PilotData *PD, Seed *Sd)
{
    BYTE BlkDir[256];
    BYTE ALignDir[256];

    PLOption *PLOP = PD->PLOP;

    for (DWORD LIndex = 0; LIndex < PLOP->SeedBlockNum; LIndex++)
    {
        DWORD Align = PLOP->SeedBlock[LIndex];
        snprintf (ALignDir, sizeof (ALignDir), "%s/Align%u", PD->CurSeedName, Align);

        DWORD BlkNum = 0;
        DWORD ValidBlkNum = 0;
        BsValue *SAList = (BsValue *) malloc (sizeof (BsValue) * (Sd->SeedLen/Align + 1));
        assert (SAList != NULL);

        DWORD LearnFailNum = 0;
        for (DWORD OFF = 0; OFF < Sd->SeedLen; OFF += Align)
        {
            BsValue *BsList = &SAList [BlkNum++];
            BsList->ValueList = NULL;
            BsList->ValueCap = BsList->ValueNum = 0;
            BsList->VarNum   = BsList->VarWeight= 0;

            if (OFF < PLOP->TryLength && IsInLearnStat(PD, OFF) == TRUE)
            {     
                snprintf (BlkDir, sizeof (BlkDir), "%s/Align%u/BLK-%u-%u", PD->CurSeedName, Align, OFF, Align);
                ReadBsList (BlkDir, BsList, Align);
                DEBUG ("@@@ [%u-%u] read value total of %u \r\n", OFF, Align, BsList->ValueNum);
            }

            /* For this seed block, we learned nothing, using original value instead */
            if (BsList->ValueNum == 0)
            {
                LearnFailNum++;
                    
                BsList->ValueList = (ULONG *)malloc (sizeof (ULONG));
                *(ULONG*)BsList->ValueList = 0;
                BsList->ValueCap  = 1;
                assert (BsList->ValueList != NULL);
                memcpy (BsList->ValueList, Sd->SeedCtx+OFF, Align);
                BsList->ValueNum++;
            }
            else
            {
                ValidBlkNum++;
            }
        }

        if (LearnFailNum == Sd->SeedLen/Align)
        {
            DEBUG ("@@@GenAllSeeds [%s]Align:%u, blocknum:%u, learning fails...!\r\n", PD->CurSeedName, Align, Sd->SeedLen/Align);        
        }
        else
        {
            ULONG *CurSeed = (ULONG *) malloc (BlkNum * sizeof (ULONG));
            assert (CurSeed != NULL);
            memset (CurSeed, 0, BlkNum * sizeof (ULONG));

            /* It is impossible to gen all seeds, we need a random sampling */
            ComputeBudget (PD, SAList, BlkNum, ValidBlkNum);

            PD->CurAlign = Align;
            PD->GenSeedNumBlock = 0;
            GenSeed (PD, SAList, BlkNum, 0, CurSeed);
            free (CurSeed);
        }

        while (BlkNum > 0)
        {
            free (SAList[--BlkNum].ValueList);        
        }
        free (SAList);   
    }

    DumpSdStat (PD);
    
    return;
}

static inline ThrData* RequirThrRes ()
{
    PLServer *plSrv    = &g_plSrv;
    
    ThrResrc *LnRes = &plSrv->LearnThrs;
    DWORD RequiredThrs;
    ThrData* Td = NULL;
    
    while (TRUE)
    {
        mutex_lock (&LnRes->RnLock);
        if (LnRes->RequiredNum < plSrv->PLOP.LnThrNum)
        {
            Td = LnRes->TD;
            for (DWORD ix = 0; ix < plSrv->PLOP.LnThrNum; ix++)
            {
                if (Td->Status == 0)
                {
                    Td->Status = 1;
                    Td->LearnThrs = (BYTE *)LnRes;
                    break;
                }
            }
            LnRes->RequiredNum++;
        }
        else
        {
            Td = NULL;
        }
        mutex_unlock (&LnRes->RnLock);

        if (Td != NULL)
        {
            break;
        }

        sleep (1);
    }

    DEBUG ("@@@ RequirThrRes: RequiredNum=%u \r\n", LnRes->RequiredNum);
    return Td;
}


static inline VOID RenderThrRes (ThrData* Td)
{
    ThrResrc *LnRes = (ThrResrc *)Td->LearnThrs;
    mutex_lock (&LnRes->RnLock);
    Td->Status = 0;
    assert (LnRes->RequiredNum > 0);
    LnRes->RequiredNum--;
    mutex_unlock (&LnRes->RnLock);
    return;
}


static inline VOID WaitForTraining ()
{
    ThrResrc *LnRes = &g_plSrv.LearnThrs;
    DWORD RequiredThrs = 0;
    
    while (TRUE)
    {
        mutex_lock (&LnRes->RnLock);
        RequiredThrs = LnRes->RequiredNum;
        mutex_unlock (&LnRes->RnLock);

        if (RequiredThrs == 0)
        {
            break;
        }
        else
        {
            sleep (1);
        }
    }
    return;
}


void* TrainingThread (void *Para)
{
    BYTE Cmd[1024];

    ThrData *Td = (ThrData *)Para;
    if (Td->BvDir == NULL)
    {
        snprintf (Cmd, sizeof (Cmd), "python -m regrnl %s -d 0.5", Td->TrainFile);
    }
    else
    {
        snprintf (Cmd, sizeof (Cmd), "python -m regrnl -B %s %s -d 0.5", Td->BvDir, Td->TrainFile);
    }
    DEBUG ("TrainingThread -> %s \r\n", Cmd);
    int Ret = system (Cmd);
    assert (Ret >= 0);

    RenderThrRes (Td);
}


static inline VOID StartTraining (PilotData *PD, BYTE *DataFile, SeedBlock *SdBlk)
{
    ThrData *Td = RequirThrRes();
    printf (">>>[StartTraining] %s --- [%u-%u]\r\n", DataFile, SdBlk->SIndex, SdBlk->Length);
    
    strncpy (Td->TrainFile, DataFile, sizeof (Td->TrainFile));
    memcpy (&Td->SdBlk, SdBlk, sizeof (SeedBlock));
    Td->BvDir = PD->PLOP->BvDir;
    
    pthread_t Tid = 0;
    int Ret = pthread_create(&Tid, NULL, TrainingThread, Td);
    if (Ret != 0)
    {
        fprintf (stderr, "pthread_create for training fail, Ret = %d\r\n", Ret);
        exit (0);
    }

    return;
}

static inline List* GetAndResetFLSeedList ()
{
    PLServer *plSrv = &g_plSrv;
    List* FlSeedList;

    mutex_lock(&plSrv->FlSdLock);
    FlSeedList = plSrv->FLSdList;
    plSrv->FLSdList = NULL;   
    mutex_unlock(&plSrv->FlSdLock);

    return FlSeedList;  
}

static inline VOID LearningMain (PilotData *PD)
{
    BYTE BlkDir[1024];
    BYTE ALignDir[1024];

    DbHandle *DHL = PD->DHL;
    DWORD SeedBlkNum = QueryDataNum (DHL->DBSeedBlockHandle);
    DWORD VarKeyNum  = QueryDataNum (DHL->DBCacheBrVarKeyHandle);

    List *FlSdList = PD->FlSdList;
    LNode *LNSeed  = FlSdList->Header;
    DWORD SeedNum  = FlSdList->NodeNum;
    
    printf ("SeedNum = %u, SeedBlkNum = %u, VarKeyNum = %u \r\n", SeedNum, SeedBlkNum, VarKeyNum);
    for (DWORD Index = 0; Index < SeedNum; Index++, LNSeed = LNSeed->Nxt)
    {
        Seed *Sd = (Seed*) LNSeed->Data;
        if (Sd->LearnStatus == LS_DONE)
        {
            continue;
        }

        BYTE* SdName = GetSeedName(Sd->SName);
        MakeDir (SdName);
        PD->CurSeedName = SdName;
        
        printf ("[LearningMain]SEED[ID-%u]%s-[length-%u] \r\n", Index, SdName, Sd->SeedLen);

        DWORD SdBlkNo = 0;
        LNode *SbHdr  = Sd->SdBlkList.Header;
        while (SbHdr != NULL)
        {
            SeedBlock *SdBlk = (SeedBlock*)SbHdr->Data;
            if (IsInLearnStat (PD, SdBlk->SIndex) == FALSE)
            {
                SdBlkNo++;
                SbHdr = SbHdr->Nxt;
                continue;
            }

            snprintf (ALignDir, sizeof (ALignDir), "%s/Align%u", SdName, SdBlk->Length);
            MakeDir (ALignDir);
            
            snprintf (BlkDir, sizeof (BlkDir), "%s/Align%u/BLK-%u-%u", SdName, SdBlk->Length, SdBlk->SIndex, SdBlk->Length);
            printf ("\t@@@ [%u]%s\r\n", SdBlkNo, BlkDir);

            for (DWORD KeyId = 1; KeyId <= VarKeyNum; KeyId++)
            {
                DWORD VarKey = *(DWORD*) GetDataByID (DHL->DBCacheBrVarKeyHandle, KeyId);
                BYTE *DataFile = GenAnalysicData (PD, BlkDir, SdBlk, VarKey);
                if (DataFile == NULL)
                {
                    continue;
                }
                printf ("\t@@@ VarKey: %x - %u -> %s\r\n", VarKey, VarKey, DataFile);

                ///// training proc here
                StartTraining (PD, DataFile, SdBlk);
            }

            SdBlkNo++;
            SbHdr = SbHdr->Nxt;
        }

        WaitForTraining ();

        printf ("[LearningMain]SEED[ID-%u]%s-[length-%u] start  GenAllSeeds...\r\n", Index, SdName, Sd->SeedLen);
        MakeDir(GEN_SEED);
        GenAllSeeds (PD, Sd);       
        WaitFuzzingOnFly (PD, TRUE);

        ListDel(&Sd->SdBlkList, NULL);
        Sd->LearnStatus = LS_DONE;
    }

    printf ("[ple]LearningMain exit, Learned seeds: %u....\r\n", PD->GenSeedNum);
    return;
}

static inline VOID GenSamplings (PilotData *PD, Seed* CurSeed, MsgIB *MsgItr)
{
    ULONG SbVal;
    SeedBlock* SBlk = AddSeedBlock (PD, CurSeed, MsgItr);
    assert (SBlk != NULL);
    SBlk->SIndex = MsgItr->SIndex;
    SBlk->Length = MsgItr->Length;
    PD->CurSdBlk = SBlk;
    
    for (DWORD Index = 0; Index < MsgItr->SampleNum; Index++)
    {
        //DEBUG ("\t@@@@ [ple-sblk][%u]:%u\r\n", Index, (DWORD)SbVal);
                        
        switch (MsgItr->Length)
        {
            case 1:
            {
                SbVal = random ()%256;
                BYTE *ValHdr = (BYTE*) (MsgItr + 1);
                ValHdr [Index] = (BYTE)SbVal;
                break;
            }
            case 2:
            {
                SbVal = random ()%256;
                WORD *ValHdr = (WORD*) (MsgItr + 1);
                ValHdr [Index] = (WORD)SbVal;
                break;
            }
            case 4:
            {
                SbVal = random ()%256;
                DWORD *ValHdr = (DWORD*) (MsgItr + 1);
                ValHdr [Index] = (DWORD)SbVal;
                break;
            }
            case 8:
            {
                SbVal = random ()%1024;
                ULONG *ValHdr = (ULONG*) (MsgItr + 1);
                ValHdr [Index] = (ULONG)SbVal;
                break;
            }
            default:
            {
                assert (0);
            }
        }

        SBlk->Value [Index] = SbVal;
    }

    return;
}

static inline VOID DeShm ()
{
    int SharedId = shmget(0xC3B3C5D0, 0, 0666);
    if (SharedId < 0)
    {
        return;
    }
    printf ("[DeShm] Delete SharedId of %d \r\n", SharedId);
    shmctl(SharedId, IPC_RMID, 0);
    return;
}



VOID PLDeInit (PLServer *plSrv)
{
    printf ("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@  PLDeInit  @@@@@@@@@@@@@@@@@@@@@@@@@@@\r\n");
    printf ("Generate total seeds: %u \r\n", plSrv->PD.GenSeedNum);
    printf ("Touched total branch variables: %u \r\n", QueryDataNum (DB_TYPE_BR_VARIABLE_KEY_CACHE));
    printf ("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\r\n\r\n");
    
    DumpBlockStat (&plSrv->PD);
    DelQueue ();
    close (plSrv->SkInfo.SockFd);
    DelDb();

    return;
}

static VOID SigHandle(int sig)
{
    PLServer *plSrv = &g_plSrv;

    printf ("[SigHandle] exit...........\r\n");
    ShowQueue(10);
    PLDeInit(plSrv);
    sem_destroy(&plSrv->PD.CollectStartSem);
    sem_destroy(&plSrv->PD.CollectEndSem);
    return;
}


VOID PLInit (PLServer *plSrv, PLOption *PLOP)
{
    DWORD Ret;
    
    DeShm ();
    memcpy (&plSrv->PLOP, PLOP, sizeof (PLOption));

    /* multi-thread init */
    ThrResrc *LearnThrs = &plSrv->LearnThrs;
    memset (LearnThrs, 0, sizeof (ThrResrc));
    mutex_lock_init (&LearnThrs->RnLock);
    LearnThrs->RequiredNum = 0;

    /* init seed block in PLOP */
    PLOP = &plSrv->PLOP;
    memset (PLOP->SeedBlock, 0, sizeof (PLOP->SeedBlock));
    PLOP->SeedBlockNum = 0;
    DWORD Bits = PLOP->SdPattBits;
    if (Bits == 0)
    {
        PLOP->SeedBlock[PLOP->SeedBlockNum++] = 1;
        PLOP->SeedBlock[PLOP->SeedBlockNum++] = 2;
        PLOP->SeedBlock[PLOP->SeedBlockNum++] = 4;
        PLOP->SeedBlock[PLOP->SeedBlockNum++] = 8;
    }
    else
    {
        while (Bits != 0)
        {
            DWORD Patt = Bits%10;
            if (Patt != 1 && Patt != 2 && Patt != 4 && Patt != 8)
            {
                fprintf (stderr, "SEED partition support [1,2,4,8]!!\r\n");
                exit (0);
            }

            printf ("[PLInit]Add seed partition: %u\r\n", Patt);
            PLOP->SeedBlock[PLOP->SeedBlockNum++] = Patt;
            Bits = Bits/10;
        }
    }

    /* init pilot data */
    PilotData *PD = &plSrv->PD;
    PD->SrvState   = SRV_S_STARTUP;
    PD->DHL        = &plSrv->DHL;
    PD->PLOP       = &plSrv->PLOP;
    LoadBlockStat (PD);
    
    PD->GenSeedNum = 0;
    PD->GenSeedNumUnit = 0;
    PD->AvgSamplingNum = 0;
    mutex_lock_init(&PD->GenSeedLock);

    Ret = sem_init(&PD->CollectStartSem, 0, 0);
    Ret|= sem_init(&PD->CollectEndSem, 0, 0);
    assert (Ret == 0);

    /* init standard data */
    StanddData *SD = &plSrv->SD;
    SD->SrvState   = SRV_S_STARTUP;
    SD->DHL        = &plSrv->DHL;
    SD->PLOP       = &plSrv->PLOP;
    
    /* init msg server */
    Ret = SrvInit (&plSrv->SkInfo);
    assert (Ret == R_SUCCESS);

    /* init DB */
    InitDbTable (plSrv);   

    /* init event queue */
    InitQueue(MEMMOD_SHARE);

    /* set default run-mode */
    plSrv->RunMode = RUNMOD_STANDD;

    mutex_lock_init(&plSrv->FlSdLock);
    plSrv->FLSdList = NULL;

    signal(SIGINT, SigHandle);

    return;
}


static inline VOID SwitchMode (RUNMOD RunMode)
{
    PLServer *plSrv = &g_plSrv;
    if (RunMode == RUNMOD_PILOT)
    {
        plSrv->PD.SrvState = SRV_S_SEEDSEND;
    }
    plSrv->RunMode = RunMode;
    return;
}


/////////////////////////////////////////////////////////////////////////////////////////////////////////
/// Pilot-fuzzing mode: for pattern learning
/////////////////////////////////////////////////////////////////////////////////////////////////////////
void* LearningMainThread (void *Para)
{
    PilotData *PD = (PilotData *)Para;
    
    LearningMain (PD);
    ListDel(PD->FlSdList, NULL);
    ResetTable(PD->DHL->DBBrVariableHandle);
    
    PD->FlSdList = NULL;
    PD->PilotStatus = PILOT_ST_IDLE;
    
    return NULL;
}

static inline DWORD GetPilotStatus ()
{
    PLServer *plSrv = &g_plSrv;
    return plSrv->PD.PilotStatus;
}

static inline DWORD PilotMode (PilotData *PD, SocketInfo *SkInfo)
{
    Seed *CurSeed;
    MsgHdr *MsgRev;
    MsgHdr *MsgSend;
    PLOption *PLOP = PD->PLOP;
    
    List *FlSdList = GetAndResetFLSeedList ();
    assert (FlSdList != NULL);
    PD->FlSdList = FlSdList;
    
    LNode *LNSeed  = FlSdList->Header;
    if (LNSeed == NULL)
    {
        printf ("[PilotMode] Warning: entry pilot-mode with no seeds for learning.\r\n");
        return FALSE;
    }

    /* create data collection thread */
    PD->PilotStatus = PILOT_ST_CLOOECTING;
    pthread_t CollectThrId = CollectBrVariables (PD);

    DWORD SeedNo  = 0;
    DWORD IsExit  = FALSE;
    while (!IsExit)
    {
        switch (PD->SrvState)
        {
            case SRV_S_SEEDSEND:
            {
                if (LNSeed == NULL)
                {
                    PD->SrvState = SRV_S_FIN;
                    break;
                }

                CurSeed = (Seed *)LNSeed->Data;
                CurSeed->SeedCtx = ReadFile (CurSeed->SName, &CurSeed->SeedLen, PLOP->SdType);
                PD->CurSeed = CurSeed;          

                MsgSend  = FormatMsg(SkInfo, PL_MSG_SEED);              
                MsgSeed *MsgSd = (MsgSeed*) (MsgSend + 1);
                MsgSd->SeedKey = CurSeed->SeedKey;
                char* SeedName = (char*) (MsgSd + 1);
                MsgSd->SeedLength = sprintf (SeedName, "%s", CurSeed->SName);
                
                MsgSend->MsgLen += sizeof (MsgSeed) + MsgSd->SeedLength;
                Send (SkInfo, (BYTE*)MsgSend, MsgSend->MsgLen);
                printf ("[PilotMode][%u/%u] send PL_MSG_SEED: [%u]%s[LENGTH:%u]\r\n", 
                        SeedNo, FlSdList->NodeNum, CurSeed->SeedKey, CurSeed->SName, CurSeed->SeedLen);
                        
                PD->SrvState = SRV_S_ITB;
                LNSeed = LNSeed->Nxt;
                SeedNo++;
                break;
            }
            case SRV_S_ITB:
            {
                CurSeed = PD->CurSeed;
                assert (CurSeed != NULL);
                
                MsgSend = FormatMsg(SkInfo, PL_MSG_ITR_BEGIN);
                MsgIB *MsgItr = (MsgIB *) (MsgSend + 1);
                MsgItr->SampleNum = PD->PLOP->SampleNum;

                DWORD OFF = 0;
                DWORD TryLength = (PLOP->TryLength < CurSeed->SeedLen) 
                                  ? PLOP->TryLength
                                  : CurSeed->SeedLen;
                for (DWORD LIndex = 0; LIndex < PLOP->SeedBlockNum; LIndex++)
                {
                    MsgItr->Length = PLOP->SeedBlock[LIndex];
                    if (MsgItr->Length > TryLength)
                    {
                        continue;
                    }
                            
                    OFF = 0;
                    while (OFF < TryLength)
                    {
                        if (IsInLearnStat (PD, OFF) == FALSE)
                        {
                            OFF += MsgItr->Length;
                            continue;
                        }
                        
                        MsgItr->SIndex = OFF;
                        MsgSend->MsgLen  = sizeof (MsgHdr) + sizeof (MsgIB);
                                
                        /* generate samples by random */
                        GenSamplings (PD, CurSeed, MsgItr);
                        OFF += MsgItr->Length;
                        MsgSend->MsgLen += MsgItr->SampleNum * MsgItr->Length;
        
                        /* before the fuzzing iteration, start the thread for collecting the branch variables */
                        PD->FzExit = FALSE;
                        sem_post(&PD->CollectStartSem);
        
                        /* inform the fuzzer */
                        Send (SkInfo, (BYTE*)MsgSend, MsgSend->MsgLen);
                        DEBUG ("[PilotMode][ITB-SEND] send PL_MSG_ITR_BEGIN[MSG-LEN:%u]: OFF:%u[SEED-LEN:%u][Sample:%u]\r\n", 
                                MsgSend->MsgLen, OFF, TryLength, MsgItr->SampleNum);
        
                        MsgHdr *MsgRecv = (MsgHdr *) Recv(SkInfo);
                        assert (MsgRecv->MsgType == PL_MSG_ITR_BEGIN);
                        DEBUG ("[PilotMode][ITB-RECV] recv PL_MSG_ITR_BEGIN done[MSG-LEN:%u]: OFF:%u[SEED-LEN:%u][Sample:%u]\r\n", 
                                MsgSend->MsgLen, OFF, TryLength, MsgItr->SampleNum);

                        PD->FzExit = TRUE;
                        sem_wait(&PD->CollectEndSem);                      
                    }
                }
                        
                PD->SrvState = SRV_S_ITE;
                break;
            }
            case SRV_S_ITE:
            {
                MsgSend = FormatMsg(SkInfo, PL_MSG_ITR_END);
                Send (SkInfo, (BYTE*)MsgSend, MsgSend->MsgLen);
                DEBUG ("[PilotMode][ITE] send PL_MSG_ITR_END...\r\n");
                        
                GetLearnStat (PD);
                        
                /* change to SRV_S_SEEDSEND, wait for next seed */
                PD->SrvState = SRV_S_SEEDSEND;
                break;
            }
            case SRV_S_FIN:
            {
                MsgSend = FormatMsg(SkInfo, PL_MSG_FZ_FIN);
                Send (SkInfo, (BYTE*)MsgSend, MsgSend->MsgLen);
                printf ("[PilotMode][FIN] send PL_MSG_FZ_FIN...\r\n");
  
                /* set the PILOT as busy: not switch to PILOT until free (FALSE) */
                PD->PilotStatus = PILOT_ST_LEARNING; 

                /* wait the collection thread to exit */
                VOID *TRet = NULL;
                sem_post(&PD->CollectStartSem);
                sem_wait(&PD->CollectEndSem);
                pthread_join (CollectThrId, &TRet);

                /* start a thread for trainning */
                pthread_t Tid = 0;
                int Ret = pthread_create(&Tid, NULL, LearningMainThread, PD);
                if (Ret != 0)
                {
                    fprintf (stderr, "pthread_create for LearningMainThread fail, Ret = %d\r\n", Ret);
                    exit (0);
                }                
                
                SwitchMode (RUNMOD_STANDD);
                IsExit = TRUE;
                break;
            }
            default:
            {
                assert (0);
            }
        }
    }
    
    return FALSE;
}


/////////////////////////////////////////////////////////////////////////////////////////////////////////
/// Standard-fuzzing mode: for standard fuzzing
/////////////////////////////////////////////////////////////////////////////////////////////////////////
static inline DWORD CacheSeedToLearn (Seed *CurSeed)
{
    DWORD CacheSdNum;
    PLServer *plSrv = &g_plSrv;
    
    mutex_lock(&plSrv->FlSdLock);
    if (plSrv->FLSdList == NULL)
    {
        plSrv->FLSdList = ListAllot ();   
    }

    if (CurSeed->LearnStatus == LS_NONE)
    {
        ListInsert(plSrv->FLSdList, CurSeed);
        CurSeed->LearnStatus = LS_READY;
    }
    CacheSdNum = plSrv->FLSdList->NodeNum;
    mutex_unlock(&plSrv->FlSdLock);

    return CacheSdNum;
}

static inline DWORD HasSeedToLearn ()
{
    PLServer *plSrv = &g_plSrv;
    DWORD SeedNum;
    
    mutex_lock(&plSrv->FlSdLock);
    if (plSrv->FLSdList == NULL)
    {
        SeedNum = 0;
    }
    else
    {
        SeedNum = plSrv->FLSdList->NodeNum;
    }
    mutex_unlock(&plSrv->FlSdLock);

    return SeedNum;
}


void* DEMonitor (void *Para)
{
    StanddData *SD = (StanddData*)Para;
    DbHandle *DHL = SD->DHL;
    DWORD CacheSdNum = 0;
    DWORD QSize = QueueSize ();
    
    DEBUG ("[DEMonitor] entry with QUEUE size: %u\r\n", QSize);  
    DWORD BrVarChg = 0;
    while (SD->FzExit == FALSE || QSize != 0)
    {
        QNode *QN = FrontQueue ();
        if (QN == NULL || QN->IsReady == FALSE)
        {
            if (QN && (time (NULL) - QN->TimeStamp >= 1))
            {
                OutQueue (QN);
            }
            QSize = QueueSize ();
            continue;
        }

        if (QN->TrcKey == TARGET_EXIT_KEY)
        {     
            if (BrVarChg != 0)
            {
                ExitInfo *ExtI = (ExitInfo*)QN->Buf;
                Seed *CurSeed = GetSeedByKey (ExtI->SeedKey);
                if (CurSeed != NULL)
                {
                    CurSeed->BrVarChg += BrVarChg; 
                    CacheSdNum = CacheSeedToLearn (CurSeed);
                    DEBUG ("[DEMonitor][SKey-%u]%s, Where:%u -> BrVarChg:%u, CacheSdNum:%u\r\n", 
                            ExtI->SeedKey, CurSeed->SName, ExtI->Where, CurSeed->BrVarChg, CacheSdNum);
                }
                else
                {
                    printf ("[DEMonitor][Warning][SKey-%u]Where:%u, Get seed fail!!! -> BrVarChg:%u\r\n", ExtI->SeedKey, ExtI->Where, BrVarChg);
                }
                
                BrVarChg = 0;
            }
        }
        else
        {
            ObjValue *OV = (ObjValue *)QN->Buf;
            BrVarChg += AddBrVarKey (SD->DHL->DBBrVarKeyHandle, QN->TrcKey);
        }
        
        OutQueue (QN);
        QSize = QueueSize ();
        
    }   

    printf ("[DEMonitor] exit with QUEUE size: %u, CacheSdNum = %u \r\n", QSize, CacheSdNum);
    pthread_exit ((void*)0);
}

static inline DWORD StandardMode (StanddData *SD, SocketInfo *SkInfo)
{
    /* Inform monitor not to exit. */
    SD->FzExit = FALSE;

    pthread_t DemId = 0;
    DWORD Ret = pthread_create(&DemId, NULL, DEMonitor, SD);
    if (Ret != 0)
    {
        fprintf (stderr, "pthread_create fail, Ret = %d\r\n", Ret);
        return TRUE;
    }

    MsgHdr *MsgRev;
    MsgHdr *MsgSend;
    Seed *CurSeed;
    while (TRUE)
    {
        /* keep recving seed from fuzzer */
        MsgRev = (MsgHdr *) Recv(SkInfo);
        switch (MsgRev->MsgType)
        {
            case PL_MSG_SEED:
            {
                MsgSeed *MsgSd = (MsgSeed*) (MsgRev + 1);
                BYTE* SeedPath = (BYTE*)(MsgSd + 1);  
                CurSeed = AddSeed (SeedPath, MsgSd->SeedKey);                
                DEBUG ("[StandardMode] recv PL_MSG_SEED: [SKey-%u]%s\r\n", CurSeed->SeedKey, SeedPath);
                break;
            }
            case PL_MSG_EMPTY:
            {
                DEBUG ("[StandardMode] recv PL_MSG_EMPTY..\r\n");
                break;
            }
            case PL_MSG_GEN_SEED:
            case PL_MSG_GEN_SEED_DONE:
            {
                DEBUG ("[StandardMode] recv PL_MSG_GEN_SEED..\r\n");
                break;
            }
            default:
            {
                assert (0);
            }
        }

        /* check for-learn seed list */
        DWORD SeedNum = HasSeedToLearn ();
        DWORD PilotSt = GetPilotStatus ();
        if (SeedNum != 0 &&  PilotSt == PILOT_ST_IDLE)
        {
            /* inform fuzzer to switch mode */
            MsgSend = FormatMsg(SkInfo, PL_MSG_SWMODE);
            Send (SkInfo, (BYTE*)MsgSend, MsgSend->MsgLen);
            DEBUG ("[StandardMode] send PL_MSG_SWMODE to fuzzer...");

            /* wait for the end of current fuzzing */
            MsgRev = (MsgHdr *) Recv(SkInfo);
            assert (MsgRev->MsgType == PL_MSG_SWMODE_READY);

            /* infor MONITOR to exit when QUEUE becomes EMPTY */
            SD->FzExit = TRUE;

            DEBUG ("[StandardMode] waiting Demonitor to exit...");
            VOID *TRet = NULL;
            pthread_join (DemId, &TRet);

            SwitchMode(RUNMOD_PILOT);
            DEBUG ("[StandardMode] OBTAIN %u [Total:%u] seeds ready to learn, switch to PILOT\r\n", 
            SeedNum, QueryDataNum (SD->DHL->DBSeedHandle));
            
            break;
        }
        else
        {
            switch (PilotSt)
            {
                case PILOT_ST_IDLE:
                case PILOT_ST_LEARNING:
                {
                    MsgSend = FormatMsg(SkInfo, MsgRev->MsgType);
                    Send (SkInfo, (BYTE*)MsgSend, MsgSend->MsgLen);
                    DEBUG ("[StandardMode] send PL_MSG_SEED[2] PL_MSG_EMPTY[8]: %u ..\r\n", MsgRev->MsgType);
                    break;
                }
                case PILOT_ST_SEEDING:
                {
                    SendSeedsForFuzzing ();
                    break;
                }
                default:
                {
                    assert (0);
                }
            }

        }        
    }
    
    return FALSE;
}


static inline VOID HandShake (PLServer *plSrv)
{
    MsgHdr *MsgRev = (MsgHdr *) Recv(&plSrv->SkInfo);
    assert (MsgRev->MsgType == PL_MSG_STARTUP);
    printf ("[HandShake] recv PL_MSG_STARTUP from Fuzzer...\r\n");

    MsgHdr *MsgSend = FormatMsg (&plSrv->SkInfo, PL_MSG_STARTUP);
    MsgHandShake *MsgHs = (MsgHandShake *)(MsgSend + 1);
    MsgHs->RunMode = plSrv->RunMode;
    MsgSend->MsgLen += sizeof (MsgHandShake);   
    Send (&plSrv->SkInfo, (BYTE*)MsgSend, MsgSend->MsgLen);
    printf ("[HandShake] reply PL_MSG_STARTUP to Fuzzer and complete handshake...\r\n");

    /* swtich the SM of PD and SD */
    plSrv->PD.SrvState = SRV_S_SEEDSEND;
    plSrv->SD.SrvState = SRV_S_SEEDSEND;

    /* !!!! clear the queue: AFL++'s validation will cause redundant events */
    ClearQueue();

    return;       
}

VOID SemanticLearning (BYTE* SeedDir, BYTE* DriverDir, PLOption *PLOP)
{
    PLServer *plSrv    = &g_plSrv;
    SocketInfo *SkInfo = &plSrv->SkInfo;
    
    PLInit (plSrv, PLOP);
    
    pthread_t Tid = 0;
    DWORD Ret = pthread_create(&Tid, NULL, FuzzingProc, DriverDir);
    if (Ret != 0)
    {
        fprintf (stderr, "pthread_create fail, Ret = %d\r\n", Ret);
        return;
    }
    
    DWORD IsExit      = FALSE;
    DWORD IsHandShake = FALSE;
    while (!IsExit)
    {
        if (IsHandShake == FALSE)
        {
            HandShake (plSrv);
            IsHandShake = TRUE;
        }

        switch (plSrv->RunMode)
        {
            case RUNMOD_PILOT:
            {
                printf ("[SemanticLearning] run mode: RUNMOD_PILOT \r\n");
                IsExit = PilotMode (&plSrv->PD, SkInfo);
                break;
            }
            case RUNMOD_STANDD:
            {
                printf ("[SemanticLearning] run mode: RUNMOD_STANDD \r\n");
                IsExit = StandardMode (&plSrv->SD, SkInfo);
                break;
            }
            default:
            {
                assert (0);
            }
        }
    }

    PLDeInit (plSrv);
    return;
}



