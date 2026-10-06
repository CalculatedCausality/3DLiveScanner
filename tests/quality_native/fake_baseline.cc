// Host-only original JNI ABI fixture; no scanner implementation or geometry.
#include <jni.h>
#include <cassert>
#include <cstdio>
#include <string>
static int connects=0,draws=0,pauses=0,saves=0,textures=0;
static std::string text(JNIEnv* env,jbyteArray data) {
    std::string value(size_t(env->GetArrayLength(data)),'\0');
    env->GetByteArrayRegion(data,0,jsize(value.size()),reinterpret_cast<jbyte*>(&value[0]));return value;
}
extern "C" {
JNIEXPORT jboolean JNICALL Java_com_lvonasek_arcore3dscanner_main_JNI_onARServiceConnected(JNIEnv* env,jclass,jobject,
        jdouble res,jdouble min,jdouble max,jint noise,jboolean holes,jboolean correction,jboolean distortion,
        jboolean offset,jboolean flashlight,jint mode,jboolean clearing,jbyteArray path) {
    assert(res==.02&&min==.01&&max==4&&noise==9&&!holes&&!correction&&!distortion&&offset&&!flashlight&&mode==1&&clearing);
    assert(!text(env,path).empty());++connects;return JNI_TRUE;
}
JNIEXPORT jboolean JNICALL Java_com_lvonasek_arcore3dscanner_main_JNI_onGlSurfaceDrawFrame(JNIEnv*,jclass,
        jboolean face,jfloat yaw,jint mode,jboolean anchors,jboolean grid,jboolean smooth) {
    assert(!face&&yaw==12.5f&&mode==2&&!anchors&&grid&&smooth);++draws;return JNI_TRUE;
}
JNIEXPORT void JNICALL Java_com_lvonasek_arcore3dscanner_main_JNI_onPause(JNIEnv*,jclass){++pauses;}
JNIEXPORT jboolean JNICALL Java_com_lvonasek_arcore3dscanner_main_JNI_save(JNIEnv* env,jclass,jbyteArray path) {
    ++saves;FILE* file=std::fopen(text(env,path).c_str(),"wb");if(!file)return JNI_FALSE;
    std::fputs("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n",file);return std::fclose(file)==0;
}
JNIEXPORT void JNICALL Java_com_lvonasek_arcore3dscanner_main_JNI_texturize(JNIEnv* env,jclass,jbyteArray input,jbyteArray output,jboolean p,jboolean t) {
    assert(!p&&!t);++textures;
    if(text(env,input)=="fail")return;
    FILE* file=std::fopen(text(env,output).c_str(),"wb");assert(file);
    std::fputs("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n",file);std::fclose(file);
}
JNIEXPORT jint JNICALL Java_com_lvonasek_arcore3dscanner_main_JNI_getScanSize(JNIEnv*,jclass){return connects*10000+draws*1000+pauses*100+saves*10+textures;}
}
