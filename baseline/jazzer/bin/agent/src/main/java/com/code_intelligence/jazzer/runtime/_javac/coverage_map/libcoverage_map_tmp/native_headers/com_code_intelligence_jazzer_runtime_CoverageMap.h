/* DO NOT EDIT THIS FILE - it is machine generated */
#include <jni.h>
/* Header for class com_code_intelligence_jazzer_runtime_CoverageMap */

#ifndef _Included_com_code_intelligence_jazzer_runtime_CoverageMap
#define _Included_com_code_intelligence_jazzer_runtime_CoverageMap
#ifdef __cplusplus
extern "C" {
#endif
#undef com_code_intelligence_jazzer_runtime_CoverageMap_INITIAL_NUM_COUNTERS
#define com_code_intelligence_jazzer_runtime_CoverageMap_INITIAL_NUM_COUNTERS 512L
/*
 * Class:     com_code_intelligence_jazzer_runtime_CoverageMap
 * Method:    initialize
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_com_code_1intelligence_jazzer_runtime_CoverageMap_initialize
  (JNIEnv *, jclass, jlong);

/*
 * Class:     com_code_intelligence_jazzer_runtime_CoverageMap
 * Method:    registerNewCounters
 * Signature: (II)V
 */
JNIEXPORT void JNICALL Java_com_code_1intelligence_jazzer_runtime_CoverageMap_registerNewCounters
  (JNIEnv *, jclass, jint, jint);

#ifdef __cplusplus
}
#endif
#endif
