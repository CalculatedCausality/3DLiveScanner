// SPDX-License-Identifier: Apache-2.0
// JNI-only compatibility layer. No C++ objects, points, pixels or poses cross
// this boundary: the validated capture/reconstruction binary stays unchanged.
#include <jni.h>
#include <dlfcn.h>
#include <android/log.h>
#include <atomic>
#include <cerrno>
#include <mutex>
#include <string>
#include <sys/stat.h>

namespace {
void* baseline=nullptr; // process-lifetime library mapping
std::atomic<bool> foreground{false};
std::mutex stateMutex;
std::mutex lifecycleMutex;
std::string datasetPath,lastError;
using Connect=jboolean(*)(JNIEnv*,jclass,jobject,jdouble,jdouble,jdouble,jint,
        jboolean,jboolean,jboolean,jboolean,jboolean,jint,jboolean,jbyteArray);
using Draw=jboolean(*)(JNIEnv*,jclass,jboolean,jfloat,jint,jboolean,jboolean,jboolean);
using Pause=void(*)(JNIEnv*,jclass);
using Save=jboolean(*)(JNIEnv*,jclass,jbyteArray);
using Texture=void(*)(JNIEnv*,jclass,jbyteArray,jbyteArray,jboolean,jboolean);
Connect connectOriginal=nullptr;Draw drawOriginal=nullptr;Pause pauseOriginal=nullptr;
Save saveOriginal=nullptr;Texture textureOriginal=nullptr;

template<class T> bool resolve(T& target,const char* name) {
    std::string symbol="Java_com_lvonasek_arcore3dscanner_main_JNI_";symbol+=name;
    target=reinterpret_cast<T>(dlsym(baseline,symbol.c_str()));return target!=nullptr;
}
bool bytes(JNIEnv* env,jbyteArray value,std::string& result) {
    if(!value)return false;
    const jsize length=env->GetArrayLength(value);
    if(length<=0||length>8192)return false;
    result.resize(size_t(length));env->GetByteArrayRegion(value,0,length,reinterpret_cast<jbyte*>(&result[0]));
    return !env->ExceptionCheck()&&result.find('\0')==std::string::npos;
}
void error(const char* message){std::lock_guard<std::mutex> lock(stateMutex);lastError=message;}
jboolean connect(JNIEnv* env,jclass cls,jobject context,jdouble res,jdouble min,jdouble max,jint noise,
        jboolean holes,jboolean correction,jboolean distortion,jboolean offset,jboolean flashlight,
        jint mode,jboolean clearing,jbyteArray path,jbyteArray) {
    try {
        std::string directory;if(!bytes(env,path,directory))return JNI_FALSE;
        {std::lock_guard<std::mutex> lock(stateMutex);datasetPath.clear();}
        const auto result=connectOriginal(env,cls,context,res,min,max,noise,holes,correction,distortion,offset,flashlight,mode,clearing,path);
        if(result==JNI_TRUE&&!env->ExceptionCheck()) {
            std::lock_guard<std::mutex> lock(stateMutex);datasetPath=directory;
            __android_log_print(ANDROID_LOG_INFO,"ScannerQuality","Original capture bridge connected; res=%.3f range=%.2f clearing=%d offset=%d",res,max,int(clearing),int(offset));
        }
        return result;
    } catch(...) {return JNI_FALSE;}
}
jboolean draw(JNIEnv* env,jclass cls,jboolean face,jfloat yaw,jint mode,jboolean anchors,jboolean grid,jboolean smooth) {
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    return foreground.load()?drawOriginal(env,cls,face,yaw,mode,anchors,grid,smooth):JNI_FALSE;
}
void resume(JNIEnv*,jclass){foreground.store(true);}
void pause(JNIEnv* env,jclass cls){std::lock_guard<std::mutex> lock(lifecycleMutex);foreground.store(false);pauseOriginal(env,cls);}
void disabledOption(JNIEnv* env,jclass,jboolean enabled) {
    if(enabled)env->ThrowNew(env->FindClass("java/lang/UnsupportedOperationException"),"This comparison preserves the original capture and reconstruction path");
}
jstring depthStatus(JNIEnv* env,jclass){return env->NewStringUTF("Original-engine candidate: neural correction disabled");}
jstring selfTest(JNIEnv* env,jclass){return env->NewStringUTF("No neural model is enabled in the original-engine comparison");}
jstring diagnostics(JNIEnv* env,jclass) {
    std::lock_guard<std::mutex> lock(stateMutex);
    return env->NewStringUTF(datasetPath.empty()?"Original native binaries; capture not started":"Original native binaries; owned capture workspace active");
}
jboolean finish(JNIEnv* env,jclass cls) {
    try {
        std::string path;
        {std::lock_guard<std::mutex> lock(stateMutex);if(datasetPath.empty())return JNI_FALSE;path=datasetPath+"/model.obj";}
        // Preserve the baseline's real native save/binder barrier. Do not invent
        // a successful finalization for a binary without finishCapture().
        auto name=env->NewByteArray(jsize(path.size()));if(!name)return JNI_FALSE;
        env->SetByteArrayRegion(name,0,jsize(path.size()),reinterpret_cast<const jbyte*>(path.data()));
        if(env->ExceptionCheck()){env->DeleteLocalRef(name);return JNI_FALSE;}
        const auto result=saveOriginal(env,cls,name);env->DeleteLocalRef(name);return result;
    } catch(...) {return JNI_FALSE;}
}
jboolean texturize(JNIEnv* env,jclass cls,jbyteArray input,jbyteArray output,jboolean poisson,jboolean twoPass) {
    try {
        std::string path;if(!bytes(env,output,path)){error("Invalid output path");return JNI_FALSE;}
        struct stat before{};
        if(lstat(path.c_str(),&before)==0||errno!=ENOENT){error("Texturing destination already exists or is inaccessible");return JNI_FALSE;}
        error("");textureOriginal(env,cls,input,output,poisson,twoPass);
        struct stat after{};
        if(env->ExceptionCheck()||stat(path.c_str(),&after)!=0||!S_ISREG(after.st_mode)||after.st_size<=0) {
            error("Original texturer did not produce the requested output; source retained");return JNI_FALSE;
        }
        return JNI_TRUE;
    } catch(...) {error("Original texturer failed");return JNI_FALSE;}
}
jstring texturingError(JNIEnv* env,jclass){std::lock_guard<std::mutex> lock(stateMutex);return env->NewStringUTF(lastError.c_str());}
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm,void*) {
    JNIEnv* env=nullptr;if(vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)!=JNI_OK)return JNI_ERR;
    baseline=dlopen("lib3dscanner.so",RTLD_NOW|RTLD_LOCAL);
    if(!baseline||!resolve(connectOriginal,"onARServiceConnected")||!resolve(drawOriginal,"onGlSurfaceDrawFrame")||
       !resolve(pauseOriginal,"onPause")||!resolve(saveOriginal,"save")||!resolve(textureOriginal,"texturize"))return JNI_ERR;
    jclass cls=env->FindClass("com/lvonasek/arcore3dscanner/main/JNI");if(!cls)return JNI_ERR;
#define ENTRY(name,signature,function) {const_cast<char*>(name),const_cast<char*>(signature),reinterpret_cast<void*>(function)}
    const JNINativeMethod methods[]={
        ENTRY("onARServiceConnected","(Landroid/content/Context;DDDIZZZZZIZ[B[B)Z",connect),
        ENTRY("onGlSurfaceDrawFrame","(ZFIZZZ)Z",draw),
        ENTRY("onResume","()V",resume),ENTRY("onPause","()V",pause),
        ENTRY("setExperimentalDepth","(Z)V",disabledOption),ENTRY("setCoveragePreview","(Z)V",disabledOption),
        ENTRY("getExperimentalDepthStatus","()Ljava/lang/String;",depthStatus),
        ENTRY("getCaptureDiagnostics","()Ljava/lang/String;",diagnostics),
        ENTRY("testExperimentalDepthRuntime","()Ljava/lang/String;",selfTest),
        ENTRY("finishCapture","()Z",finish),ENTRY("texturize","([B[BZZ)Z",texturize),
        ENTRY("getTexturingError","()Ljava/lang/String;",texturingError)
    };
#undef ENTRY
    const int result=env->RegisterNatives(cls,methods,sizeof(methods)/sizeof(methods[0]));env->DeleteLocalRef(cls);
    return result==0?JNI_VERSION_1_6:JNI_ERR;
}
