
#include "DynTrace.h"
#include "JCovPCG_DynTrace.h"

#ifdef __cplusplus
extern "C" {
#endif


/*
 * Class:     JCovPCG_DynTrace
 * Method:    JvTrace
 * Signature: (I)V
 */
JNIEXPORT void JNICALL Java_JCovPCG_DynTrace_JvTrace (JNIEnv *env, jobject jobj, jint guard)
{
    unsigned TrcKey = guard;
    DynTrace (NULL, 0, TrcKey);
    return;
}


/*
 * Class:     JCovPCG_DynTrace
 * Method:    JvTraceInit
 * Signature: (I)V
 */
JNIEXPORT void JNICALL Java_JCovPCG_DynTrace_JvTraceInit (JNIEnv *env, jobject jobj, jint blockNum)
{
    unsigned BBs = blockNum;
    DynTraceInit (BBs);
    return;
}

/*
* Class:     JCovPCG_DynTrace
* Method:    JvTraceDeInit
* Signature: (I)V
*/
JNIEXPORT void JNICALL Java_JCovPCG_DynTrace_JvTraceDeInit (JNIEnv *env, jobject jobj, jint exitCode)
{
    /* exit anyway */
    exit (exitCode);
}


#ifdef __cplusplus
}
#endif




