/*
 *  videoExternal.cpp
 *  artoolkitX
 *
 *  This file is part of artoolkitX.
 *
 *  artoolkitX is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  artoolkitX is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with artoolkitX.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  As a special exception, the copyright holders of this library give you
 *  permission to link this library with independent modules to produce an
 *  executable, regardless of the license terms of these independent modules, and to
 *  copy and distribute the resulting executable under terms of your choice,
 *  provided that you also meet, for each linked independent module, the terms and
 *  conditions of the license of that module. An independent module is a module
 *  which is neither derived from nor based on this library. If you modify this
 *  library, you may extend this exception to your version of the library, but you
 *  are not obligated to do so. If you do not wish to do so, delete this exception
 *  statement from your version.
 *
 *  Copyright 2023 Philip Lamb
 *
 *  Author(s): Philip Lamb
 *
 */


#include "videoExternal.h"

#ifdef ARVIDEO_INPUT_EXTERNAL

#include <string.h> // memset()
#include <pthread.h>
#include <ARX/ARUtil/time.h>
#include <ARX/ARUtil/system.h>
#include <ARX/ARVideo/videoRGBA.h>
#include "../cparamSearch.h"
#include <ARX/ARUtil/system.h>

typedef enum {
    ARVideoExternalIncomingPixelFormat_UNKNOWN,
    ARVideoExternalIncomingPixelFormat_NV21,
    ARVideoExternalIncomingPixelFormat_NV12,
    ARVideoExternalIncomingPixelFormat_RGBA,
    ARVideoExternalIncomingPixelFormat_RGB_565,
    ARVideoExternalIncomingPixelFormat_YUV_420_888, // In Android, frames with this format are actually NV21.
    ARVideoExternalIncomingPixelFormat_MONO,
    ARVideoExternalIncomingPixelFormat_RGBA_5551,
    ARVideoExternalIncomingPixelFormat_RGBA_4444
} ARVideoExternalIncomingPixelFormat;

struct _AR2VideoParamExternalT {
    // Frame-related.
    int                width; // Width of incoming frames, set in pushInit.
    int                height; // height of incoming frames, set in pushInit.
    ARVideoExternalIncomingPixelFormat incomingPixelFormat; // Format of incoming frames, set in pushInit.
    AR_PIXEL_FORMAT    pixelFormat; // ARToolKit equivalent format of incoming frames, set in pushInit.
    int                convertToRGBA;
    // cparamSearch-related.
    float              focal_length; // metres.
    int                cameraIndex; // 0 = first camera, 1 = second etc.
    int                cameraPosition; // enum AR_VIDEO_POSITION_*
    char              *device_id;
    void             (*cparamSearchCallback)(const ARParam *, void *);
    void              *cparamSearchUserdata;
    // Opening-related.
    void             (*openAsyncCallback)(void *);
    void              *openAsyncUserdata;
    bool               openingAsync; // true when openAsync is active. If set to false, indicates video closed and it will cleanup.
    //
    pthread_mutex_t    frameLock;  // Protects: capturing, pushInited, pushNewFrameReady.
    pthread_cond_t     pushInitedCond; // Condition variable used to block openAsync from returning until least one frame received (or close is called).
    bool               pushInited; // videoPushInit called.
    bool               capturing; // Between capStart and capStop.
    bool               pushNewFrameReady; // New frame ready since last arVideoGetImage.
    AR2VideoBufferT    buffers[2];
    bool               copy;
    // Only valid when copy is true.
    bool               copyYWarning;
    bool               copyUVWarning;
    // Only valid when copy is false.
    int                bufferCheckoutState; // Only valid when copy is false; <0=no buffers checked out. 0=buffers[0] checked out, 1=buffers[1] checked out.
    void             (*releaseCallbacks[2])(void *);
    void              *releaseCallbacksUserdata[2];
    // Others.
};

static void cleanupVid(AR2VideoParamExternalT *vid);
static void *openAsyncThread(void *arg);

int ar2VideoDispOptionExternal( void )
{
    ARPRINT(" -module=External\n");
    ARPRINT("\n");
    ARPRINT(" -format=[0|RGBA].\n");
    ARPRINT("    Specifies the pixel format for output images.\n");
    ARPRINT("    0=use system default. RGBA=output RGBA, including conversion if necessary.\n");
    ARPRINT(" -nocopy\n");
    ARPRINT("    Don't copy frames, but instead hold a reference to the frame data. The caller\n");
    ARPRINT("    must keep the frame data valid until the release callback is called, or capture is stopped.\n");
    ARPRINT(" -cachedir=/path/to/cparam_cache.db\n");
    ARPRINT("    Specifies the path in which to look for/store camera parameter cache files.\n");
    ARPRINT("    Default is app's cache directory, or on Android a folder 'cparam_cache' in the current working directory.\n");
    ARPRINT(" -cacheinitdir=/path/to/cparam_cache_init.db\n");
    ARPRINT("    Specifies the path in which to look for/store initial camera parameter cache file.\n");
    ARPRINT("    Default is app's bundle directory, or on Android a folder 'cparam_cache' in the current working directory.\n");
    ARPRINT(" -deviceid=string (or -deviceid=\"string with whitespace\") Override device ID used for.\n");
    ARPRINT("    camera parameters search, on platforms where cparamSearch is available.\n");
    ARPRINT("\n");

    return 0;
}

AR2VideoParamExternalT *ar2VideoOpenAsyncExternal(const char *config, void (*callback)(void *), void *userdata)
{
    char                     *cacheDir = NULL;
    char                     *cacheInitDir = NULL;
    char                     *csdu = NULL;
    char                     *csat = NULL;
    AR2VideoParamExternalT   *vid;
    const char               *a;
    char                      line[1024];
    int err_i = 0;
    int i;
    int width = 0, height = 0;
    int convertToRGBA = 0;
    bool copy = true;

    arMallocClear(vid, AR2VideoParamExternalT, 1);

    a = config;
    if (a != NULL) {
        for(;;) {
            while(*a == ' ' || *a == '\t') a++;
            if (*a == '\0') break;

            if (sscanf(a, "%s", line) == 0) break;
            if (strcmp(line, "-module=External") == 0) {
            } else if (strcmp(line, "-copy") == 0) {
                copy = true;
            } else if (strcmp(line, "-nocopy") == 0) {
                copy = false;
            } else if (strncmp(line, "-width=", 7) == 0) {
                if (sscanf(&line[7], "%d", &width) == 0) {
                    ARLOGe("Error: Configuration option '-width=' must be followed by width in integer pixels.\n");
                    err_i = 1;
                }
            } else if (strncmp(line, "-height=", 8) == 0) {
                if (sscanf(&line[8], "%d", &height) == 0) {
                    ARLOGe("Error: Configuration option '-height=' must be followed by height in integer pixels.\n");
                    err_i = 1;
                }
            } else if (strncmp(line, "-format=", 8) == 0) {
                if (strcmp(line+8, "0") == 0) {
                    convertToRGBA = 0;
                    ARLOGi("Requesting images in system default format.\n");
                } else if (strcmp(line+8, "RGBA") == 0) {
                    convertToRGBA = 1;
                    ARLOGi("Requesting images in RGBA format.\n");
                } else {
                    ARLOGe("Ignoring unsupported request for conversion to video format '%s'.\n", line+8);
                }
            } else if (strncmp(a, "-cachedir=", 10) == 0) {
                // Attempt to read in pathname, allowing for quoting of whitespace.
                a += 10; // Skip "-cachedir=" characters.
                if (*a == '"') {
                    a++;
                    // Read all characters up to next '"'.
                    i = 0;
                    while (i < (sizeof(line) - 1) && *a != '\0') {
                        line[i] = *a;
                        a++;
                        if (line[i] == '"') break;
                        i++;
                    }
                    line[i] = '\0';
                } else {
                    sscanf(a, "%s", line);
                }
                if (!strlen(line)) {
                    ARLOGe("Error: Configuration option '-cachedir=' must be followed by path (optionally in double quotes).\n");
                    err_i = 1;
                } else {
                    free(cacheDir);
                    cacheDir = strdup(line);
                }
            } else if (strncmp(a, "-cacheinitdir=", 14) == 0) {
                // Attempt to read in pathname, allowing for quoting of whitespace.
                a += 14; // Skip "-cacheinitdir=" characters.
                if (*a == '"') {
                    a++;
                    // Read all characters up to next '"'.
                    i = 0;
                    while (i < (sizeof(line) - 1) && *a != '\0') {
                        line[i] = *a;
                        a++;
                        if (line[i] == '"') break;
                        i++;
                    }
                    line[i] = '\0';
                } else {
                    sscanf(a, "%s", line);
                }
                if (!strlen(line)) {
                    ARLOGe("Error: Configuration option '-cacheinitdir=' must be followed by path (optionally in double quotes).\n");
                    err_i = 1;
                } else {
                    free(cacheInitDir);
                    cacheInitDir = strdup(line);
                }
            } else if (strncmp(a, "-csdu=", 6) == 0) {
                // Attempt to read in download URL.
                a += 6; // Skip "-csdu=" characters.
                sscanf(a, "%s", line);
                free(csdu);
                if (!strlen(line)) {
                    csdu = NULL;
                } else {
                    csdu = strdup(line);
                }
            } else if (strncmp(a, "-csat=", 6) == 0) {
                // Attempt to read in authentication token, allowing for quoting of whitespace.
                a += 6; // Skip "-csat=" characters.
                if (*a == '"') {
                    a++;
                    // Read all characters up to next '"'.
                    i = 0;
                    while (i < (sizeof(line) - 1) && *a != '\0') {
                        line[i] = *a;
                        a++;
                        if (line[i] == '"') break;
                        i++;
                    }
                    line[i] = '\0';
                } else {
                    sscanf(a, "%s", line);
                }
                free(csat);
                if (!strlen(line)) {
                    csat = NULL;
                } else {
                    csat = strdup(line);
                }
            } else if (strncmp(a, "-deviceid=", 10) == 0) {
                // Attempt to read in authentication token, allowing for quoting of whitespace.
                a += 10; // Skip "-deviceid=" characters.
                if (*a == '"') {
                    a++;
                    // Read all characters up to next '"'.
                    i = 0;
                    while (i < (sizeof(line) - 1) && *a != '\0') {
                        line[i] = *a;
                        a++;
                        if (line[i] == '"') break;
                        i++;
                    }
                    line[i] = '\0';
                } else {
                    sscanf(a, "%s", line);
                }
                free(vid->device_id);
                if (!strlen(line)) {
                    vid->device_id = NULL;
                } else {
                    vid->device_id = strdup(line);
                }
            } else {
                 err_i = 1;
            }

            if (err_i) {
                ARLOGe("Error: Unrecognised configuration option '%s'.\n", a);
                ar2VideoDispOptionExternal();
                goto bail;
            }

            while (*a != ' ' && *a != '\t' && *a != '\0') a++;
        }
    }

    // Check for option compatibility.
    if (width != 0 || height != 0) {
        ARLOGw("Warning: Video frame size is determined by pushed video. Configuration options '-width=' and '-height=' will be ignored.\n");
    }

#if USE_CPARAM_SEARCH
    // Initialisation required before cparamSearch can be used.
#if !ARX_TARGET_PLATFORM_ANDROID
    if (!cacheDir) {
        cacheDir = arUtilGetResourcesDirectoryPath(AR_UTIL_RESOURCES_DIRECTORY_BEHAVIOR_USE_APP_CACHE_DIR);
    }
    if (!cacheInitDir) {
        cacheInitDir = arUtilGetResourcesDirectoryPath(AR_UTIL_RESOURCES_DIRECTORY_BEHAVIOR_BEST); // Bundle dir on iOS and macOS, exe dir on windows, exedir/../share/exename/ on Linux, cache dir on Android.
    }
#  endif
    // Initialisation required before cparamSearch can be used.
    if (cparamSearchInit(cacheDir ? cacheDir : "cparam_cache", cacheInitDir ? cacheInitDir : "cparam_cache", false, csdu, csat) < 0) {
        ARLOGe("Unable to initialise cparamSearch.\n");
        goto bail;
    };
#endif

    // Initial state.
    vid->incomingPixelFormat = ARVideoExternalIncomingPixelFormat_UNKNOWN;
    vid->pixelFormat = AR_PIXEL_FORMAT_INVALID;
    vid->convertToRGBA = convertToRGBA;
    if (!vid->focal_length) vid->focal_length = AR_VIDEO_EXTERNAL_FOCAL_LENGTH_DEFAULT;
    vid->pushInited = false;
    vid->openingAsync = false;
    vid->capturing = false;
    vid->pushNewFrameReady = false;
    vid->cameraIndex = -1;
    vid->cameraPosition = AR_VIDEO_POSITION_UNKNOWN;
    vid->openAsyncCallback = callback;
    vid->openAsyncUserdata = userdata;
    vid->copy = copy;
    vid->copyYWarning = false;
    vid->copyUVWarning = false;

#if ARX_TARGET_PLATFORM_ANDROID || ARX_TARGET_PLATFORM_IOS
    if (!vid->device_id) {
        // In lieu of identifying the actual camera, we use manufacturer/model/board to identify a device,
        // and assume that identical devices have identical cameras.
        vid->device_id = arUtilGetDeviceID();
    }
#endif

    pthread_mutex_init(&(vid->frameLock), NULL);
    pthread_cond_init(&(vid->pushInitedCond), NULL);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, 1); // Preclude the need to do pthread_join on the thread after it exits.
    pthread_t t;
    vid->openingAsync = true;
    err_i = pthread_create(&t, &attr, openAsyncThread, vid);
    pthread_attr_destroy(&attr);
    if (err_i != 0) {
        ARLOGe("ar2VideoOpenAsyncExternal(): pthread_create error %s (%d).\n", strerror(err_i), err_i);
        goto bail1;
    }

    goto done;

bail1:
#if USE_CPARAM_SEARCH
    if (cparamSearchFinal() < 0) {
        ARLOGe("Unable to finalise cparamSearch.\n");
    }
#endif
    pthread_cond_destroy(&vid->pushInitedCond);
    pthread_mutex_destroy(&vid->frameLock);
bail:
    free(vid->device_id);
    free(vid);
    vid = NULL;

done:
    free(cacheDir);
    free(cacheInitDir);
    free(csdu);
    free(csat);
    return (vid);
}

// Wait for ar2VideoPushInitExternal to be called before
// invoking user's callback, so that frame parameters (w, h etc) are known.
static void *openAsyncThread(void *arg)
{
    int err;

    AR2VideoParamExternalT *vid = (AR2VideoParamExternalT *)arg; // Cast the thread start arg to the correct type.

    pthread_mutex_lock(&(vid->frameLock));
    while (!vid->pushInited && vid->openingAsync) { // Exit the wait if EITHER a frame is pushed OR videoCloseExternal is called.
        // Android "Bionic" libc doesn't implement cancelation, so need to let wait expire somewhat regularly.
        uint64_t sec;
        uint32_t usec;
        arUtilTimeSinceEpoch(&sec, &usec);
        struct timespec ts;
        ts.tv_sec = sec + 2;
        ts.tv_nsec = usec * 1000;
        err = pthread_cond_timedwait(&(vid->pushInitedCond), &(vid->frameLock), &ts);
        if (err != ETIMEDOUT && err != 0) {
            ARLOGe("openAsyncThread(): pthread_cond_timedwait error %s (%d).\n", strerror(err), err);
            break;
        }
    }
    pthread_mutex_unlock(&(vid->frameLock));

    if (!vid->openingAsync) {
        // videoCloseExternal was called before any frames were pushed. Just cleanup and exit.
        cleanupVid(vid);
    } else {
        vid->openingAsync = false;
        (vid->openAsyncCallback)(vid->openAsyncUserdata);
    }

    return (NULL);
}

static void cleanupVid(AR2VideoParamExternalT *vid)
{
#if USE_CPARAM_SEARCH
    if (cparamSearchFinal() < 0) {
        ARLOGe("Unable to finalise cparamSearch.\n");
    }
#endif
    pthread_cond_destroy(&vid->pushInitedCond);
    pthread_mutex_destroy(&vid->frameLock);
    free(vid->device_id);
    free(vid);
}

int ar2VideoCloseExternal( AR2VideoParamExternalT *vid )
{
    if (!vid) return (-1); // Sanity check.

    bool pushInited;
    pthread_mutex_lock(&(vid->frameLock));
    pushInited = vid->pushInited;
    pthread_mutex_unlock(&(vid->frameLock));
    if (pushInited) {
        ARLOGe("Error: cannot close video while frames are still being pushed.\n");
        return (-1);
    }

    if (vid->capturing) ar2VideoCapStopExternal(vid);

    if (vid->openingAsync) {
        vid->openingAsync = false;
    } else {
        cleanupVid(vid);
    }

    return 0;
}

int ar2VideoCapStartExternal( AR2VideoParamExternalT *vid )
{
    int ret = -1;

    if (!vid) return -1; // Sanity check.

    pthread_mutex_lock(&(vid->frameLock));
    if (vid->capturing) goto done; // Already capturing.
    vid->capturing = true;
    vid->pushNewFrameReady = false;

    ret = 0;
done:
    pthread_mutex_unlock(&(vid->frameLock));
    return (ret);
}

static void releaseAndUpdate(AR2VideoParamExternalT *vid, int bufferIndex, void (*updatedReleaseCallback)(void *), void *updatedReleaseCallbackUserdata)
{
    if (vid->releaseCallbacks[bufferIndex]) {
        (*vid->releaseCallbacks[bufferIndex])(vid->releaseCallbacksUserdata[bufferIndex]);
    }
    vid->releaseCallbacks[bufferIndex] = updatedReleaseCallback;
    vid->releaseCallbacksUserdata[bufferIndex] = updatedReleaseCallbackUserdata;
}

int ar2VideoCapStopExternal( AR2VideoParamExternalT *vid )
{
    int ret = -1;

    if (!vid) return -1; // Sanity check.

    pthread_mutex_lock(&(vid->frameLock));
    if (!vid->capturing) goto done; // Not capturing.
    vid->capturing = false;
    vid->pushNewFrameReady = false;
    if (!vid->copy) {
        releaseAndUpdate(vid, 0, NULL, NULL);
        releaseAndUpdate(vid, 1, NULL, NULL);
        vid->bufferCheckoutState = -1;
    }
    ret = 0;
done:
    pthread_mutex_unlock(&(vid->frameLock));
    return (ret);
}

AR2VideoBufferT *ar2VideoGetImageExternal( AR2VideoParamExternalT *vid )
{
    AR2VideoBufferT *ret = NULL;

    if (!vid || !vid->capturing) return NULL; // Sanity check.

    pthread_mutex_lock(&(vid->frameLock));
    if (vid->pushInited && vid->pushNewFrameReady) {
        if (!vid->copy) {
            // If we have a buffer checked out, we're OK to release it now.
            if (vid->bufferCheckoutState >= 0) {
                releaseAndUpdate(vid, vid->bufferCheckoutState, NULL, NULL);
            }
            // Work out which buffer we'll be checking out. For the very first frame, it'll be
            // buffer 0, otherwise it'll be the buffer that's NOT already checked out.
            int newBufIndex = vid->bufferCheckoutState == 0 ? 1 : 0;
            vid->bufferCheckoutState = newBufIndex;
            ret = &vid->buffers[newBufIndex];
        } else {
            ret = &vid->buffers[0];
        }
        vid->pushNewFrameReady = false;
    }
    pthread_mutex_unlock(&(vid->frameLock));
    return (ret);
}

int ar2VideoGetSizeExternal(AR2VideoParamExternalT *vid, int *x,int *y)
{
    if (!vid) return (-1); // Sanity check.
    if (x) *x = vid->width;
    if (y) *y = vid->height;

    return 0;
}

AR_PIXEL_FORMAT ar2VideoGetPixelFormatExternal( AR2VideoParamExternalT *vid )
{
    if (!vid) return AR_PIXEL_FORMAT_INVALID; // Sanity check.
    
    if (vid->convertToRGBA) {
        return (AR_PIXEL_FORMAT_RGBA);
    } else {
        return vid->pixelFormat;
    }
}

int ar2VideoGetIdExternal( AR2VideoParamExternalT *vid, ARUint32 *id0, ARUint32 *id1 )
{
    return -1;
}

int ar2VideoGetParamiExternal( AR2VideoParamExternalT *vid, int paramName, int *value )
{
    if (!value) return -1;

    switch (paramName) {
        case AR_VIDEO_PARAM_GET_IMAGE_ASYNC:
            *value = 0;
            break;
        case AR_VIDEO_PARAM_ANDROID_CAMERA_INDEX:
            *value = vid->cameraIndex;
            break;
        case AR_VIDEO_PARAM_ANDROID_CAMERA_FACE:
            // Translate to the Android equivalent.
            *value = (vid->cameraPosition == AR_VIDEO_POSITION_FRONT ? AR_VIDEO_ANDROID_CAMERA_FACE_FRONT : AR_VIDEO_ANDROID_CAMERA_FACE_REAR);
            break;
        case AR_VIDEO_PARAM_AVFOUNDATION_CAMERA_POSITION:
            // Translate to the AVFoundation equivalent.
            switch (vid->cameraPosition) {
                case AR_VIDEO_POSITION_BACK:
                    *value = AR_VIDEO_AVFOUNDATION_CAMERA_POSITION_REAR;
                    break;
                case AR_VIDEO_POSITION_FRONT:
                    *value = AR_VIDEO_AVFOUNDATION_CAMERA_POSITION_FRONT;
                    break;
                case AR_VIDEO_POSITION_UNKNOWN:
                    *value = AR_VIDEO_AVFOUNDATION_CAMERA_POSITION_UNKNOWN;
                    break;
                default:
                    *value = AR_VIDEO_AVFOUNDATION_CAMERA_POSITION_UNSPECIFIED;
                    break;
             }
             break;
        case AR_VIDEO_PARAM_AVFOUNDATION_FOCUS_PRESET:
            // Translate to the AVFoundation equivalent.
            if (vid->focal_length <= 0.0) {
                *value = AR_VIDEO_AVFOUNDATION_FOCUS_NONE;
            } else if (vid->focal_length > 6.0) {
                *value = AR_VIDEO_AVFOUNDATION_FOCUS_INF;
            } else if (vid->focal_length < 0.05) {
                *value = AR_VIDEO_AVFOUNDATION_FOCUS_MACRO;
            } else if (vid->focal_length > 0.5) {
                *value = AR_VIDEO_AVFOUNDATION_FOCUS_1_0M;
            } else {
                *value = AR_VIDEO_AVFOUNDATION_FOCUS_0_3M;
            }
            break;
        default:
            return (-1);
    }

    return -1;
}

int ar2VideoSetParamiExternal( AR2VideoParamExternalT *vid, int paramName, int  value )
{
    if (!vid) return (-1); // Sanity check.

    switch (paramName) {
        case AR_VIDEO_PARAM_AVFOUNDATION_FOCUS_PRESET:
            // Translate to metres.
            switch (value) {
                case AR_VIDEO_AVFOUNDATION_FOCUS_INF:
                    vid->focal_length = INFINITY;
                    break;
                case AR_VIDEO_AVFOUNDATION_FOCUS_1_0M:
                    vid->focal_length = 1.0f;
                    break;
                case AR_VIDEO_AVFOUNDATION_FOCUS_MACRO:
                    vid->focal_length = 0.01f;
                    break;
                case AR_VIDEO_AVFOUNDATION_FOCUS_0_3M:
                    vid->focal_length = 0.3f;
                    break;
                case AR_VIDEO_AVFOUNDATION_FOCUS_NONE:
                default:
                    vid->focal_length = 0.0f;
                    break;
            }
            break;
       default:
            return (-1);
    }
    return (0);


    return -1;
}

int ar2VideoGetParamdExternal( AR2VideoParamExternalT *vid, int paramName, double *value )
{
    if (!vid || !value) return (-1); // Sanity check.

    switch (paramName) {
        case AR_VIDEO_PARAM_ANDROID_FOCAL_LENGTH:   *value = (double)vid->focal_length; break;
        default:
            return (-1);
    }
    return (0);
}

int ar2VideoSetParamdExternal( AR2VideoParamExternalT *vid, int paramName, double  value )
{
    if (!vid) return (-1); // Sanity check.

    switch (paramName) {
        case AR_VIDEO_PARAM_ANDROID_FOCAL_LENGTH:   vid->focal_length = (float)value; break;
        default:
            return (-1);
    }
    return (0);
}

int ar2VideoGetParamsExternal( AR2VideoParamExternalT *vid, const int paramName, char **value )
{
    if (!vid || !value) return (-1); // Sanity check.
    
    switch (paramName) {
        case AR_VIDEO_PARAM_DEVICEID:               *value = strdup(vid->device_id); break;
        default:
            return (-1);
    }
    return (0);
}

#if USE_CPARAM_SEARCH
static void cparamSeachCallback(CPARAM_SEARCH_STATE state, float progress, const ARParam *cparam, void *userdata)
{
    int final = false;
    AR2VideoParamExternalT *vid = (AR2VideoParamExternalT *)userdata;
    if (!vid) return;

    switch (state) {
        case CPARAM_SEARCH_STATE_INITIAL:
        case CPARAM_SEARCH_STATE_IN_PROGRESS:
            break;
        case CPARAM_SEARCH_STATE_RESULT_NULL:
            if (vid->cparamSearchCallback) (*vid->cparamSearchCallback)(NULL, vid->cparamSearchUserdata);
            final = true;
            break;
        case CPARAM_SEARCH_STATE_OK:
            if (vid->cparamSearchCallback) (*vid->cparamSearchCallback)(cparam, vid->cparamSearchUserdata);
            final = true;
            break;
        case CPARAM_SEARCH_STATE_FAILED_NO_NETWORK:
            ARLOGe("Error during cparamSearch. Internet connection unavailable.\n");
            if (vid->cparamSearchCallback) (*vid->cparamSearchCallback)(NULL, vid->cparamSearchUserdata);
            final = true;
            break;
        default: // Errors.
            ARLOGe("Error %d returned from cparamSearch.\n", (int)state);
            if (vid->cparamSearchCallback) (*vid->cparamSearchCallback)(NULL, vid->cparamSearchUserdata);
            final = true;
            break;
    }
    if (final) {
        vid->cparamSearchCallback = (void (*)(const ARParam *, void *))nullptr;
        vid->cparamSearchUserdata = nullptr;
    }
}

int ar2VideoGetCParamAsyncExternal(AR2VideoParamExternalT *vid, void (*callback)(const ARParam *, void *), void *userdata)
{
    if (!vid) return (-1); // Sanity check.
    if (!callback) {
        ARLOGw("Warning: cparamSearch requested without callback.\n");
    }

    vid->cparamSearchCallback = callback;
    vid->cparamSearchUserdata = userdata;

    CPARAM_SEARCH_STATE initialState = cparamSearch(vid->device_id, vid->cameraIndex, vid->width, vid->height, vid->focal_length, &cparamSeachCallback, (void *)vid);
    if (initialState != CPARAM_SEARCH_STATE_INITIAL) {
        ARLOGe("Error %d returned from cparamSearch.\n", initialState);
        vid->cparamSearchCallback = (void (*)(const ARParam *, void *))nullptr;
        vid->cparamSearchUserdata = nullptr;
        return (-1);
    }

    return (0);
}
#endif

int ar2VideoSetParamsExternal( AR2VideoParamExternalT *vid, const int paramName, const char  *value )
{
    if (!vid) return (-1);
    
    switch (paramName) {
        case AR_VIDEO_PARAM_DEVICEID:
            free (vid->device_id);
            if (value) vid->device_id = strdup(value);
            else vid->device_id = NULL;
            break;
        default:
            return (-1);
    }
    return (0);
}

int ar2VideoPushInitExternal(AR2VideoParamExternalT *vid, int width, int height, const char *pixelFormat, int cameraIndex, int cameraPosition)
{
    int err;
    int ret = -1;

    ARLOGd("ar2VideoPushInitExternal(): %s camera at %dx%d (%s).\n", (cameraPosition == AR_VIDEO_POSITION_FRONT ? "front" : "back"), width, height, pixelFormat);

    if (!vid || width <= 0 || height <= 0 || !pixelFormat) return (-1); // Sanity check.

    pthread_mutex_lock(&(vid->frameLock));

    if (vid->pushInited) {
        ARLOGe("ar2VideoPushInitExternal: Error: called while already inited.\n");
        goto done;
    }

    if (strcmp(pixelFormat, "NV21") == 0) {
        vid->incomingPixelFormat = ARVideoExternalIncomingPixelFormat_NV21;
        vid->pixelFormat = AR_PIXEL_FORMAT_NV21;
    } else if (strcmp(pixelFormat, "NV12") == 0) {
        vid->incomingPixelFormat = ARVideoExternalIncomingPixelFormat_NV12;
        vid->pixelFormat = AR_PIXEL_FORMAT_420f;
    } else if (strcmp(pixelFormat, "YUV_420_888") == 0) {
        vid->incomingPixelFormat = ARVideoExternalIncomingPixelFormat_YUV_420_888;
        // We will convert!
        vid->pixelFormat = AR_PIXEL_FORMAT_NV21;
    } else if (strcmp(pixelFormat, "RGBA") == 0)  {
        vid->incomingPixelFormat = ARVideoExternalIncomingPixelFormat_RGBA;
        vid->pixelFormat = AR_PIXEL_FORMAT_RGBA;
    } else if (strcmp(pixelFormat, "MONO") == 0)  {
        vid->incomingPixelFormat = ARVideoExternalIncomingPixelFormat_MONO;
        vid->pixelFormat = AR_PIXEL_FORMAT_MONO;
    } else if (strcmp(pixelFormat, "RGB_565") == 0)  {
        vid->incomingPixelFormat = ARVideoExternalIncomingPixelFormat_RGB_565;
        vid->pixelFormat = AR_PIXEL_FORMAT_RGB_565;
    } else if (strcmp(pixelFormat, "RGBA_5551") == 0)  {
        vid->incomingPixelFormat = ARVideoExternalIncomingPixelFormat_RGBA_5551;
        vid->pixelFormat = AR_PIXEL_FORMAT_RGBA_5551;
    } else if (strcmp(pixelFormat, "RGBA_4444") == 0)  {
        vid->incomingPixelFormat = ARVideoExternalIncomingPixelFormat_RGBA_4444;
        vid->pixelFormat = AR_PIXEL_FORMAT_RGBA_4444;
    } else {
        ARLOGe("ar2VideoPushInitExternal: Error: frames arriving in unsupported pixel format '%s'.\n", pixelFormat);
        goto done;
    }

    // Prepare the vid->buffer structure.
    if (!vid->copy) {
        for (int i = 0; i < 2; i++) {
            if (vid->pixelFormat == AR_PIXEL_FORMAT_NV21 || vid->pixelFormat == AR_PIXEL_FORMAT_420f) {
                vid->buffers[i].bufPlaneCount = 2;
                vid->buffers[i].bufPlanes = (ARUint8 **)calloc(vid->buffers[i].bufPlaneCount, sizeof(ARUint8 *));
                if (vid->convertToRGBA) vid->buffers[i].buff = (ARUint8 *)malloc(width * height * 4);
            } else {
                vid->buffers[i].bufPlaneCount = 0;
                vid->buffers[i].bufPlanes = NULL;
            }
            vid->buffers[i].buff = NULL;
            vid->buffers[i].buffLuma = NULL;
        }
        vid->bufferCheckoutState = -1;
    } else {
        if (vid->pixelFormat == AR_PIXEL_FORMAT_NV21 || vid->pixelFormat == AR_PIXEL_FORMAT_420f) {
            vid->buffers[0].bufPlaneCount = 2;
            vid->buffers[0].bufPlanes = (ARUint8 **)calloc(vid->buffers[0].bufPlaneCount, sizeof(ARUint8 *));
            vid->buffers[0].bufPlanes[0] = (ARUint8 *)malloc(width * height);
            vid->buffers[0].bufPlanes[1] = (ARUint8 *)malloc(2 * (width / 2 * height / 2));
            vid->buffers[0].buffLuma = vid->buffers[0].bufPlanes[0];
            if (vid->convertToRGBA) {
                vid->buffers[0].buff = (ARUint8 *)malloc(width * height * 4);
            } else {
                vid->buffers[0].buff = vid->buffers[0].bufPlanes[0];
            }
        } else {
            vid->buffers[0].bufPlaneCount = 0;
            vid->buffers[0].bufPlanes = NULL;
            if (vid->pixelFormat == AR_PIXEL_FORMAT_RGBA) {
                vid->buffers[0].buff = (ARUint8 *)malloc(width * height * 4);
            } else if (vid->pixelFormat == AR_PIXEL_FORMAT_RGB_565 || vid->pixelFormat == AR_PIXEL_FORMAT_RGBA_5551 || vid->pixelFormat == AR_PIXEL_FORMAT_RGBA_4444) {
                vid->buffers[0].buff = (ARUint8 *)malloc(width * height * 2);
            } else if (vid->pixelFormat == AR_PIXEL_FORMAT_MONO) {
                vid->buffers[0].buff = (ARUint8 *)malloc(width * height);
            }
            vid->buffers[0].buffLuma = NULL;
        }
    }
    vid->width = width;
    vid->height = height;
    vid->cameraIndex = cameraIndex;
    vid->cameraPosition = cameraPosition;
    vid->pushInited = true;
    ret = 0;

done:
    pthread_mutex_unlock(&(vid->frameLock));
    // Signal that pushInit has been called. This will unblock the openAsync thread and
    // the callback from that thread.
    if ((err = pthread_cond_signal(&(vid->pushInitedCond))) != 0) {
        ARLOGe("ar2VideoPushInitExternal(): pthread_cond_signal error %s (%d).\n", strerror(err), err);
    }
    return (ret);

}

int ar2VideoPushExternal(AR2VideoParamExternalT *vid,
                         ARUint8 *buf0p, int buf0Size, int buf0PixelStride, int buf0RowStride,
                         ARUint8 *buf1p, int buf1Size, int buf1PixelStride, int buf1RowStride,
                         ARUint8 *buf2p, int buf2Size, int buf2PixelStride, int buf2RowStride,
                         ARUint8 *buf3p, int buf3Size, int buf3PixelStride, int buf3RowStride,
                         void (*releaseCallback)(void *), void *releaseCallbackUserdata)
{
    if (!vid) return -1; // Sanity check.
    int ret = -1;

    //ARLOGd("ar2VideoPushExternal(buf0p=%p, buf0Size=%d, buf0PixelStride=%d, buf0RowStride=%d, buf1p=%p, buf1Size=%d, buf1PixelStride=%d, buf1RowStride=%d, buf2p=%p, buf2Size=%d, buf2PixelStride=%d, buf2RowStride=%d, buf3p=%p, buf3Size=%d, buf3PixelStride=%d, buf3RowStride=%d)\n", buf0p, buf0Size, buf0PixelStride, buf0RowStride, buf1p, buf1Size, buf1PixelStride, buf1RowStride, buf2p, buf2Size, buf2PixelStride, buf2RowStride, buf3p, buf3Size, buf3PixelStride, buf3RowStride);

    pthread_mutex_lock(&(vid->frameLock));

    int bufferIndex = 0;
    AR2VideoBufferT *buffer = NULL;

    if (!vid->pushInited) goto done; // Failure to call ar2VideoPushInitExternal is an error.
    if (!vid->capturing) {
        // If ar2VideoCapStartExternal has not been called, it's not an error, but we shouldn't do anything. Throw the frame away.
        if (releaseCallback) (*releaseCallback)(releaseCallbackUserdata);
        ret = 0;
        goto done;
    }
    if (!buf0p || buf0Size <= 0) {
        ARLOGe("ar2VideoPushExternal: NULL buffer.\n");
        goto done;
    }

    // If not copying, need to select destination buffer from our double-buffer set. First time around,
    // vid->bufferCheckoutState will be -1, so defaults to 0 in this case. Otherwise, the buffer not currently checked out.
    // If copying, use default buffer 0 (which will have been allocated in arVideoPushInit()).
    if (!vid->copy) bufferIndex = vid->bufferCheckoutState == 0 ? 1 : 0;
    buffer = &vid->buffers[bufferIndex];

    // Get time of capture as early as possible.
    arUtilTimeSinceEpoch(&buffer->time.sec, &buffer->time.usec);

    if (!vid->copy) {
        // If not copying, we need to release any callback saved for any previous frame pushed which hasn't yet
        // been used by videoGetImage. Release previous incoming frame, and save release details for this frame.
        releaseAndUpdate(vid, bufferIndex, releaseCallback, releaseCallbackUserdata);
    }

    // Grab the incoming frame.
    if (vid->incomingPixelFormat == ARVideoExternalIncomingPixelFormat_NV21 || vid->incomingPixelFormat == ARVideoExternalIncomingPixelFormat_NV12) {
        if (!buf1p || buf1Size <= 0) {
            ARLOGe("ar2VideoPushExternal: Error: insufficient buffers for format NV21/NV12.\n");
            goto done;
        }
        if ((vid->width * vid->height) != buf0Size || (2 * (vid->width/2 * vid->height/2)) != buf1Size) {
            ARLOGe("ar2VideoPushExternal: Error: unexpected buffer sizes (%d, %d) for format NV21/NV12.\n", buf0Size, buf1Size);
            goto done;
        }
        if (!vid->copy) {
            buffer->bufPlanes[0] = buf0p;
            buffer->bufPlanes[1] = buf1p;
        } else {
            memcpy(buffer->bufPlanes[0], buf0p, buf0Size);
            memcpy(buffer->bufPlanes[1], buf1p, buf1Size);
        }
        // Convert if the user requested RGBA.
        if (vid->convertToRGBA) {
            videoRGBA((uint32_t *)&buffer->buff, buffer, vid->width, vid->height, vid->pixelFormat);
        } else {
            buffer->buff = buffer->bufPlanes[0];
        }
        buffer->buffLuma = buffer->bufPlanes[0];

    } else if (vid->incomingPixelFormat == ARVideoExternalIncomingPixelFormat_YUV_420_888) {
        if (!buf1p || buf1Size <= 0 || !buf2p || buf2Size <= 0) {
            ARLOGe("ar2VideoPushExternal: Error: insufficient buffers for format YUV_420_888.\n");
            goto done;
        }

        if ((vid->width * vid->height) != buf0Size) {
            ARLOGe("ar2VideoPushExternal: Error: unexpected buffer size (%d) for format YUV_420_888.\n", buf0Size);
            goto done;
        }

        if (!vid->copy) {
            // Make sure it's actually NV21.
            if (buf0RowStride != vid->width || buf1PixelStride != 2 || buf2PixelStride != 2 || buf1RowStride != vid->width || buf2RowStride != vid->width || ((buf1p - 1) != buf2p)) {
                ARLOGe("ar2VideoPushExternal: Error: in nocopy mode, can't handle YUV_420_888 unless it's actually NV21.\n");
                goto done;
            }
            buffer->bufPlanes[0] = buf0p;
            buffer->bufPlanes[1] = buf2p;
        } else {
            // Massage into NV21 format.
            // Y plane first. Note that YUV_420_888 guarantees buf0PixelStride == 1.
            if (buf0RowStride == vid->width) {
                memcpy(buffer->bufPlanes[0], buf0p, buf0Size);
            } else {
                if (!vid->copyYWarning) {
                    ARLOGw("ar2VideoPushExternal: Warning: caller sent YUV_420_888 with padded rows. Slower Y copy will occur.\n");
                    vid->copyYWarning = true;
                }
                unsigned char *p = buffer->bufPlanes[0], *p0 = buf0p;
                for (int i = 0; i < vid->height; i++) {
                    memcpy(p, p0, vid->width);
                    p += vid->width;
                    p0 += buf0RowStride;
                }
            }
            // Next, U (Cb) and V (Cr) planes.
            if ((buf1PixelStride == 2 && buf2PixelStride == 2) && (buf1RowStride == vid->width && buf2RowStride == vid->width) && ((buf1p - 1) == buf2p)) {
                // U and V planes both have pixelstride of 2, rowstride of pixelstride * vid->width/2, and are interleaved by 1 byte, so it's already NV21 and we can do a direct copy.
                memcpy(buffer->bufPlanes[1], buf2p, 2*vid->width/2*vid->height/2);
            } else {
                // Tedious conversion to NV21.
                if (!vid->copyUVWarning) {
                    ARLOGw("ar2VideoPushExternal: Warning: caller sent YUV_420_888 with non-interleaved UV. Slow conversion will occur.\n");
                    vid->copyUVWarning = true;
                }
                unsigned char *p = buffer->bufPlanes[1], *p1 = buf1p, *p2 = buf2p;
                for (int i = 0; i < vid->height / 2; i++) {
                    for (int j = 0; j < vid->width / 2; j++) {
                        *p++ = p2[j * buf1PixelStride]; // Cr
                        *p++ = p1[j * buf2PixelStride]; // Cb
                    }
                    p1 += buf1RowStride;
                    p2 += buf2RowStride;
                }
            }
        }

        // Convert if the user requested RGBA.
        if (vid->convertToRGBA) {
            videoRGBA((uint32_t *)&buffer->buff, buffer, vid->width, vid->height, vid->pixelFormat);
        } else {
            buffer->buff = buffer->bufPlanes[0];
        }
        buffer->buffLuma = buffer->bufPlanes[0];

    } else if (vid->incomingPixelFormat == ARVideoExternalIncomingPixelFormat_RGBA) {
        if ((vid->width * vid->height * 4) != buf0Size) {
            ARLOGe("ar2VideoPushExternal: Error: unexpected buffer size (%d) for format RGBA.\n", buf0Size);
            goto done;
        }
        if (!vid->copy) {
            buffer->buff = buf0p;
        } else {
            memcpy(buffer->buff, buf0p, buf0Size);
        }
        buffer->buffLuma = NULL;
    } else if (vid->incomingPixelFormat == ARVideoExternalIncomingPixelFormat_MONO) {
        if ((vid->width * vid->height) != buf0Size) {
            ARLOGe("ar2VideoPushExternal: Error: unexpected buffer size (%d) for format MONO.\n", buf0Size);
            goto done;
        }
        if (!vid->copy) {
            buffer->buff = buf0p;
        } else {
            memcpy(buffer->buff, buf0p, buf0Size);
        }
        buffer->buffLuma = buffer->buff;
    } else if (vid->incomingPixelFormat == ARVideoExternalIncomingPixelFormat_RGB_565 || vid->incomingPixelFormat == ARVideoExternalIncomingPixelFormat_RGBA_5551 || vid->incomingPixelFormat == ARVideoExternalIncomingPixelFormat_RGBA_4444) {
        if ((vid->width * vid->height * 2) != buf0Size) {
            ARLOGe("ar2VideoPushExternal: Error: unexpected buffer size (%d) for format RGB_565/RGBA_5551/RGBA_4444.\n", buf0Size);
            goto done;
        }
        if (!vid->copy) {
            buffer->buff = buf0p;
        } else {
            memcpy(buffer->buff, buf0p, buf0Size);
        }
        buffer->buffLuma = NULL;
    }

    ret = 0;
    buffer->fillFlag = 1;
    vid->pushNewFrameReady = true;

done:
    pthread_mutex_unlock(&(vid->frameLock));
    
    if (vid->copy && releaseCallback) {
        // If we've copied the data and user wanted a callback to release, invoke it now.
        // (This is probably not a commonly used path, but here for sake of completeness).
        (*releaseCallback)(releaseCallbackUserdata);
    }

    return (ret);

}

int ar2VideoPushFinalExternal(AR2VideoParamExternalT *vid)
{
    ARLOGd("ar2VideoPushFinalExternal()\n");

    int ret = -1;

    if (!vid) return -1; // Sanity check.

    pthread_mutex_lock(&(vid->frameLock));

    if (!vid->pushInited) goto done;

    if (!vid->copy) {
        for (int i = 0; i < 2; i++) {
            if (vid->pixelFormat == AR_PIXEL_FORMAT_NV21 || vid->pixelFormat == AR_PIXEL_FORMAT_420f) {
                free(vid->buffers[i].bufPlanes);
                vid->buffers[i].bufPlanes = NULL;
                vid->buffers[i].bufPlaneCount = 0;
                if (vid->convertToRGBA) {
                    free(vid->buffers[i].buff);
                }
            }
            vid->buffers[i].buff = NULL;
            vid->buffers[i].buffLuma = NULL;
        }
        releaseAndUpdate(vid, 0, NULL, NULL);
        releaseAndUpdate(vid, 1, NULL, NULL);
        vid->bufferCheckoutState = -1;
    } else {
        if (vid->buffers[0].bufPlaneCount > 0) {
            for (int i = 0; i < vid->buffers[0].bufPlaneCount; i++) {
                free(vid->buffers[0].bufPlanes[i]);
                vid->buffers[0].bufPlanes[i] = NULL;
            }
            free(vid->buffers[0].bufPlanes);
            vid->buffers[0].bufPlanes = NULL;
            vid->buffers[0].bufPlaneCount = 0;
            if (vid->convertToRGBA) {
                free(vid->buffers[0].buff);
            }
        } else {
            free(vid->buffers[0].buff);
        }
        vid->buffers[0].buff = NULL;
        vid->buffers[0].buffLuma = NULL;
    }
    vid->width = vid->height = 0;
    vid->incomingPixelFormat = ARVideoExternalIncomingPixelFormat_UNKNOWN;
    vid->pushInited = false;
    vid->pushNewFrameReady = false;
    ret = 0;

done:
    pthread_mutex_unlock(&(vid->frameLock));
    return (ret);

}

#endif //  ARVIDEO_INPUT_EXTERNAL
