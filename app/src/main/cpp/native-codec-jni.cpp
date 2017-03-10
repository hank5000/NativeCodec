/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* This is a JNI example where we use native methods to play video
 * using the native AMedia* APIs.
 * See the corresponding Java source file located at:
 *
 *   src/com/example/nativecodec/NativeMedia.java
 *
 * In this example we use assert() for "impossible" error conditions,
 * and explicit handling and recovery for more likely error conditions.
 */

#include <assert.h>
#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

#include "looper.h"
#include "media/NdkMediaCodec.h"
#include "media/NdkMediaExtractor.h"

// for __android_log_print(ANDROID_LOG_INFO, "YourApp", "formatted message");
#include <android/log.h>
#define TAG "NativeCodec"
#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// for native window JNI
#include <android/native_window_jni.h>


typedef struct {
    int fd;
    ANativeWindow* window;
    AMediaExtractor* ex;
    AMediaCodec *codec;
    int64_t renderstart;
    bool sawInputEOS;
    bool sawOutputEOS;
    bool isPlaying;
    bool renderonce;
    jobject surface;
    int32_t width;
    int32_t height;
} workerdata;

workerdata data = {-1, NULL, NULL, NULL, 0, false, false, false, false, 0, 0, 0};
bool bSwap = false;

enum {
    kMsgCodecBuffer,
    kMsgPause,
    kMsgResume,
    kMsgPauseAck,
    kMsgDecodeDone,
    kMsgSeek,
};


/* Return 1 if the difference is negative, otherwise 0.  */
int timeval_subtract(struct timeval *result, struct timeval *t2, struct timeval *t1)
{
    long int diff = (t2->tv_usec + 1000000 * t2->tv_sec) - (t1->tv_usec + 1000000 * t1->tv_sec);
    result->tv_sec = diff / 1000000;
    result->tv_usec = diff % 1000000;
    LOGE("Diff Time:%d",diff);
    return (diff<0);
}


class mylooper: public looper {
    virtual void handle(int what, void* obj);
};

static mylooper *mlooper = NULL;

int64_t systemnanotime() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000000000LL + now.tv_nsec;
}

void doCodecWork(workerdata *d) {

    ssize_t bufidx = -1;
    if (!d->sawInputEOS) {
        bufidx = AMediaCodec_dequeueInputBuffer(d->codec, 2000);
//        LOGV("input buffer %zd", bufidx);
        if (bufidx >= 0) {
            size_t bufsize;
            auto buf = AMediaCodec_getInputBuffer(d->codec, bufidx, &bufsize);
            auto sampleSize = AMediaExtractor_readSampleData(d->ex, buf, bufsize);
            if (sampleSize < 0) {
                sampleSize = 0;
                d->sawInputEOS = true;
                LOGV("EOS");
            }
            auto presentationTimeUs = AMediaExtractor_getSampleTime(d->ex);

            AMediaCodec_queueInputBuffer(d->codec, bufidx, 0, sampleSize, presentationTimeUs,
                    d->sawInputEOS ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0);
            AMediaExtractor_advance(d->ex);
        }
    }

    if (!d->sawOutputEOS) {
        AMediaCodecBufferInfo info;
        int status = AMediaCodec_dequeueOutputBuffer(d->codec, &info, 0);

        if (status >= 0) {
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                LOGV("output EOS");
                d->sawOutputEOS = true;
            }
            int64_t presentationNano = info.presentationTimeUs * 1000;
            if (d->renderstart < 0) {
                d->renderstart = systemnanotime() - presentationNano;
            }
            int64_t delay = (d->renderstart + presentationNano) - systemnanotime();
            if (delay > 0) {
                usleep(delay / 1000);
            }
            size_t frame_size = 0;
            uint8_t* framePt = NULL;
            framePt = AMediaCodec_getOutputBuffer(d->codec,status,&frame_size);
            if(NULL!=framePt) {
                ANativeWindow_Buffer buffer;
                int WIDNOW_FORMAT_NV21 = 0x11;
                ANativeWindow_setBuffersGeometry(data.window,d->width,d->height,WIDNOW_FORMAT_NV21);
                if (ANativeWindow_lock(data.window, &buffer, NULL) == 0) {
                    struct timeval tvBegin, tvEnd, tvDiff;
                    gettimeofday(&tvBegin, NULL);

                    if(!bSwap) {
                        memcpy(buffer.bits, framePt, d->width * d->height * 3 / 2);
                    } else {
                        int ySize = d->width * d->height;
                        unsigned char* dest = (unsigned char*)buffer.bits;

                        memcpy(dest,framePt,ySize);

                        int uvSize = ySize>>1;
                        int uSize = uvSize>>1;
                        memcpy(dest+ySize,framePt+ySize+1,uvSize-1);
                        unsigned char *nvcur = framePt+ySize;
                        unsigned char *yuvcur = dest+ySize+1;
                        int i=0;
                        while(i<uSize)
                        {
                            (*yuvcur)=(*nvcur);
                            yuvcur+=2;
                            nvcur+=2;
                            ++i;
                        }
                    }
                    gettimeofday(&tvEnd, NULL);
                    long int diff = (tvEnd.tv_usec + 1000000 * tvEnd.tv_sec) - (tvBegin.tv_usec + 1000000 * tvBegin.tv_sec);
                    LOGE("Spend Time:%d",diff/1000);


                    ANativeWindow_unlockAndPost(data.window);
                }
            }
            AMediaCodec_releaseOutputBuffer(d->codec, status, info.size != 0);
            if (d->renderonce) {
                d->renderonce = false;
                return;
            }
        } else if (status == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            LOGV("output buffers changed");
        } else if (status == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            auto format = AMediaCodec_getOutputFormat(d->codec);
            LOGV("format changed to: %s", AMediaFormat_toString(format));
            AMediaFormat_delete(format);
        } else if (status == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            LOGV("no output buffer right now");
        } else {
            LOGV("unexpected info code: %zd", status);
        }
    }

    if (!d->sawInputEOS || !d->sawOutputEOS) {
        mlooper->post(kMsgCodecBuffer, d);
    }
}

void mylooper::handle(int what, void* obj) {
    switch (what) {
        case kMsgCodecBuffer:
            doCodecWork((workerdata*)obj);
            break;

        case kMsgDecodeDone:
        {
            workerdata *d = (workerdata*)obj;
            AMediaCodec_stop(d->codec);
            AMediaCodec_delete(d->codec);
            AMediaExtractor_delete(d->ex);
            d->sawInputEOS = true;
            d->sawOutputEOS = true;
        }
        break;

        case kMsgSeek:
        {
            workerdata *d = (workerdata*)obj;
            AMediaExtractor_seekTo(d->ex, 0, AMEDIAEXTRACTOR_SEEK_NEXT_SYNC);
            AMediaCodec_flush(d->codec);
            d->renderstart = -1;
            d->sawInputEOS = false;
            d->sawOutputEOS = false;
            if (!d->isPlaying) {
                d->renderonce = true;
                post(kMsgCodecBuffer, d);
            }
            LOGV("seeked");
        }
        break;

        case kMsgPause:
        {
            workerdata *d = (workerdata*)obj;
            if (d->isPlaying) {
                // flush all outstanding codecbuffer messages with a no-op message
                d->isPlaying = false;
                post(kMsgPauseAck, NULL, true);
            }
        }
        break;

        case kMsgResume:
        {
            workerdata *d = (workerdata*)obj;
            if (!d->isPlaying) {
                d->renderstart = -1;
                d->isPlaying = true;
                post(kMsgCodecBuffer, d);
            }
        }
        break;
    }
}




extern "C" {

static struct CachedFields {
    jclass fileDescriptorClass;
    jmethodID fileDescriptorCtor;
    jfieldID descriptorField;
} gCachedFields;

bool bInit = false;

int jniGetFdFromFileDescriptor(JNIEnv* env, jobject jFileDescriptor) {
    if(!bInit) {
        gCachedFields.fileDescriptorClass = env->FindClass("java/io/FileDescriptor");

        gCachedFields.fileDescriptorCtor =
                env->GetMethodID(gCachedFields.fileDescriptorClass, "<init>", "()V");

        gCachedFields.descriptorField =
                env->GetFieldID(gCachedFields.fileDescriptorClass, "descriptor", "I");
    }
    return env->GetIntField(jFileDescriptor, gCachedFields.descriptorField);
}

void Java_com_example_nativecodec_NativeCodec_doUVSwap(JNIEnv* env, jclass clazz, jboolean swap) {
    bSwap = swap;
}

jboolean Java_com_example_nativecodec_NativeCodec_createStreamingMediaPlayer(JNIEnv* env,
        jclass clazz, jobject jFileDescriptor)
{
    LOGV("@@@ create");

    off_t outStart = 0;
    off64_t outLen=0x7ffffffffffffffL;

    int fd = jniGetFdFromFileDescriptor(env, jFileDescriptor);

    if (fd < 0) {
        LOGE("failed to open file: %s %d (%s)", fd, strerror(errno));
        return JNI_FALSE;
    }

    data.fd = fd;

    workerdata *d = &data;

    AMediaExtractor *ex = AMediaExtractor_new();
    media_status_t err = AMediaExtractor_setDataSourceFd(ex, d->fd,
                                                         static_cast<off64_t>(outStart),
                                                         outLen);
    close(d->fd);
    if (err != AMEDIA_OK) {
        LOGV("setDataSource error: %d", err);
        return JNI_FALSE;
    }

    int numtracks = AMediaExtractor_getTrackCount(ex);

    AMediaCodec *codec = NULL;

    LOGV("input has %d tracks", numtracks);
    for (int i = 0; i < numtracks; i++) {
        AMediaFormat *format = AMediaExtractor_getTrackFormat(ex, i);
        const char *s = AMediaFormat_toString(format);
        LOGV("track %d format: %s", i, s);
        const char *mime;
        if (!AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mime)) {
            LOGV("no mime type");
            return JNI_FALSE;
        } else if (!strncmp(mime, "video/", 6)) {

            AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &data.height);
            AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &data.width);

            int64_t NV21 = 21;
            AMediaFormat_setInt64(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, NV21);
            // Omitting most error handling for clarity.
            // Production code should check for errors.
            AMediaExtractor_selectTrack(ex, i);
            codec = AMediaCodec_createDecoderByType(mime);
            AMediaCodec_configure(codec, format, NULL, NULL, 0);
            d->ex = ex;
            d->codec = codec;
            d->renderstart = -1;
            d->sawInputEOS = false;
            d->sawOutputEOS = false;
            d->isPlaying = false;
            d->renderonce = true;
            AMediaCodec_start(codec);
        }
        AMediaFormat_delete(format);
    }

    mlooper = new mylooper();
    mlooper->post(kMsgCodecBuffer, d);

    return JNI_TRUE;
}

// set the playing state for the streaming media player
void Java_com_example_nativecodec_NativeCodec_setPlayingStreamingMediaPlayer(JNIEnv* env,
        jclass clazz ,jboolean isPlaying)
{
//    workerdata* data = (workerdata*) workdata;
    LOGV("@@@ playpause: %d", isPlaying);
    if (mlooper) {
        if (isPlaying) {
            mlooper->post(kMsgResume, &data);
        } else {
            mlooper->post(kMsgPause, &data);
        }
    }
}


// shut down the native media system
void Java_com_example_nativecodec_NativeCodec_shutdown(JNIEnv* env, jclass clazz)
{
    LOGV("@@@ shutdown");
    if (mlooper) {
        mlooper->post(kMsgDecodeDone, &data, true /* flush */);
        mlooper->quit();
        delete mlooper;
        mlooper = NULL;
    }

}


// set the surface
void Java_com_example_nativecodec_NativeCodec_setSurface(JNIEnv *env, jclass clazz,jobject surface)
{

    // obtain a native window from a Java surface
    if (data.window) {
        ANativeWindow_release(data.window);
        data.window = NULL;
    }
    data.window = ANativeWindow_fromSurface(env, surface);

    LOGV("@@@ setsurface %p", data.window);
}


// rewind the streaming media player
void Java_com_example_nativecodec_NativeCodec_rewindStreamingMediaPlayer(JNIEnv *env, jclass clazz)
{
    LOGV("@@@ rewind");
    if (mlooper) {
        mlooper->post(kMsgSeek, &data);
    }
}

}
