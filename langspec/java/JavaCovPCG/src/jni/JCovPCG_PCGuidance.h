/* DO NOT EDIT THIS FILE - it is machine generated */
#include <jni.h>
/* Header for class JCovPCG_PCGuidance */

#ifndef _Included_JCovPCG_PCGuidance
#define _Included_JCovPCG_PCGuidance
#ifdef __cplusplus
extern "C" {
#endif
/*
 * Class:     JCovPCG_PCGuidance
 * Method:    pcgCFGAlloct
 * Signature: (I)I
 */
JNIEXPORT jint JNICALL Java_JCovPCG_PCGuidance_pcgCFGAlloct
  (JNIEnv *, jclass, jint);

/*
 * Class:     JCovPCG_PCGuidance
 * Method:    pcgCFGDel
 * Signature: (I)V
 */
JNIEXPORT void JNICALL Java_JCovPCG_PCGuidance_pcgCFGDel
  (JNIEnv *, jclass, jint);

/*
 * Class:     JCovPCG_PCGuidance
 * Method:    pcgCFGEdge
 * Signature: (III)V
 */
JNIEXPORT void JNICALL Java_JCovPCG_PCGuidance_pcgCFGEdge
  (JNIEnv *, jclass, jint, jint, jint);

/*
 * Class:     JCovPCG_PCGuidance
 * Method:    pcgInsertIR
 * Signature: (IILjava/lang/String;)V
 */
JNIEXPORT void JNICALL Java_JCovPCG_PCGuidance_pcgInsertIR
  (JNIEnv *, jclass, jint, jint, jstring);

/*
 * Class:     JCovPCG_PCGuidance
 * Method:    pcgBuild
 * Signature: (I)V
 */
JNIEXPORT void JNICALL Java_JCovPCG_PCGuidance_pcgBuild
  (JNIEnv *, jclass, jint);

/*
 * Class:     JCovPCG_PCGuidance
 * Method:    pcgNeedInstrumented
 * Signature: (II)Z
 */
JNIEXPORT jboolean JNICALL Java_JCovPCG_PCGuidance_pcgNeedInstrumented
  (JNIEnv *, jclass, jint, jint);

/*
 * Class:     JCovPCG_PCGuidance
 * Method:    pcgGetPCGStmtID
 * Signature: (II)I
 */
JNIEXPORT jint JNICALL Java_JCovPCG_PCGuidance_pcgGetPCGStmtID
  (JNIEnv *, jclass, jint, jint);

/*
 * Class:     JCovPCG_PCGuidance
 * Method:    pcgGetAllSAIStmtIDs
 * Signature: (I)[I
 */
JNIEXPORT jintArray JNICALL Java_JCovPCG_PCGuidance_pcgGetAllSAIStmtIDs
  (JNIEnv *, jclass, jint);

/*
 * Class:     JCovPCG_PCGuidance
 * Method:    pcgIsDominated
 * Signature: (III)Z
 */
JNIEXPORT jboolean JNICALL Java_JCovPCG_PCGuidance_pcgIsDominated
  (JNIEnv *, jclass, jint, jint, jint);

/*
 * Class:     JCovPCG_PCGuidance
 * Method:    pcgIsPostDominated
 * Signature: (III)Z
 */
JNIEXPORT jboolean JNICALL Java_JCovPCG_PCGuidance_pcgIsPostDominated
  (JNIEnv *, jclass, jint, jint, jint);

#ifdef __cplusplus
}
#endif
#endif
