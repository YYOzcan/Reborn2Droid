// Android-only glue: pull the original ARM libraries out of our own APK's assets (AAssetManager).
#ifdef __ANDROID__
#include "rt.h"
#include <SDL.h>
#include <jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <sys/stat.h>
#include <string>

std::string android_extract_libs() {
    JNIEnv *e = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = (jobject)SDL_AndroidGetActivity();
    jclass k = e->GetObjectClass(act);
    jmethodID ga = e->GetMethodID(k, "getAssets", "()Landroid/content/res/AssetManager;");
    jobject am_obj = e->CallObjectMethod(act, ga);
    AAssetManager *am = AAssetManager_fromJava(e, am_obj);
    std::string dir = std::string(SDL_AndroidGetInternalStoragePath()) + "/armlibs";
    mkdir(dir.c_str(), 0700);
    for (const char *name : {"libmono.so", "libunity.so"}) {
        std::string src = std::string("libs/armeabi-v7a/") + name, dst = dir + "/" + name;
        AAsset *a = AAssetManager_open(am, src.c_str(), AASSET_MODE_STREAMING);
        if (!a) rt::fatal("asset %s missing from the APK", src.c_str());
        off_t len = AAsset_getLength(a);
        struct stat st;
        if (stat(dst.c_str(), &st) == 0 && st.st_size == len) { AAsset_close(a); continue; }
        FILE *f = fopen((dst + ".tmp").c_str(), "wb");
        if (!f) rt::fatal("cannot write %s", dst.c_str());
        char buf[65536]; int n;
        while ((n = AAsset_read(a, buf, sizeof buf)) > 0) fwrite(buf, 1, n, f);
        fclose(f); AAsset_close(a);
        rename((dst + ".tmp").c_str(), dst.c_str());
    }
    e->DeleteLocalRef(am_obj); e->DeleteLocalRef(k); e->DeleteLocalRef(act);
    return dir;
}
#endif
