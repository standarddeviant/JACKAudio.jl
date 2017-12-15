#include "portaudio.h"
#include <pa_ringbuffer.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#define MIN(x, y) ((x) < (y) ? (x) : (y))

typedef enum {
    JACK_SHIM_ERRMSG_OVERFLOW, // input overflow
    JACK_SHIM_ERRMSG_UNDERFLOW, // output underflow
    JACK_SHIM_ERRMSG_ERR_OVERFLOW, // error buffer overflowed
} jack_shim_errmsg_t;

// this callback type is used to notify the Julia side that the jack
// callback has run
typedef void (*jack_shim_notifycb_t)(void *userdata);

// This struct is shared between the Julia side and C
typedef struct {
    PaUtilRingBuffer *inputbuf; // ringbuffer for input
    PaUtilRingBuffer *outputbuf; // ringbuffer for output
    PaUtilRingBuffer *errorbuf; // ringbuffer to send error notifications
    int sync; // keep input/output ring buffers synchronized (0/1)
    jack_shim_notifycb_t notifycb; // Julia callback to notify conditions
    void *inputhandle; // condition to notify on new input
    void *outputhandle; // condition to notify when ready for output
    void *errorhandle; // condition to notify on new error
} jack_shim_info_t;

void senderr(jack_shim_info_t *info, jack_shim_errmsg_t msg) {
    if(PaUtil_GetRingBufferWriteAvailable(info->errorbuf) < 2) {
        // we've overflowed our error buffer! notify the host.
        msg = JACK_SHIM_ERRMSG_ERR_OVERFLOW;
    }
    PaUtil_WriteRingBuffer(info->errorbuf, &msg, 1);
    if(info->notifycb) {
        info->notifycb(info->errorhandle);
    }
}

// return the sha256 hash of the shim source so we can make sure things are in sync
const char *jack_shim_getsourcehash(void)
{
    // defined on the command-line at build-time
    return SOURCEHASH;
}

/*
 * This routine will be called by the PortAudio engine when audio is needed.
 * It may called at interrupt level on some machines so don't do anything that
 * could mess up the system like calling malloc() or free().
 */
int jack_shim_processcb(const void *input, void *output,
                     unsigned long frameCount,
                     const PaStreamCallbackTimeInfo* timeInfo,
                     PaStreamCallbackFlags statusFlags,
                     void *userData)
{
    jack_shim_info_t *info = userData;
    if(info->notifycb == NULL) {
        fprintf(stderr, "jack_shim ERROR: notifycb is NULL\n");
    }
    int nwrite;
    if(info->inputbuf) {
        nwrite = PaUtil_GetRingBufferWriteAvailable(info->inputbuf);
        nwrite = MIN(frameCount, nwrite);
    }
    int nread;
    if(info->outputbuf) {
        nread = PaUtil_GetRingBufferReadAvailable(info->outputbuf);
        nread = MIN(frameCount, nread);
    }
    if(info->inputbuf && info->outputbuf && info->sync) {
        // to keep the buffers synchronized, set readable and writable to
        // their minimum value
        nread = MIN(nread, nwrite);
        nwrite = nread;
    }
    // read/write from the ringbuffers
    if(info->inputbuf) {
        PaUtil_WriteRingBuffer(info->inputbuf, input, nwrite);
        if(info->notifycb) {
            info->notifycb(info->inputhandle);
        }
        if(nwrite < frameCount) {
            senderr(info, JACK_SHIM_ERRMSG_OVERFLOW);
        }
    }
    if(info->outputbuf) {
        PaUtil_ReadRingBuffer(info->outputbuf, output, nread);
        if(info->notifycb) {
            info->notifycb(info->outputhandle);
        }
        if(nread < frameCount) {
            senderr(info, JACK_SHIM_ERRMSG_UNDERFLOW);
            // we didn't fill the whole output buffer, so zero it out
            memset(output+nread*info->outputbuf->elementSizeBytes, 0,
                   (frameCount - nread)*info->outputbuf->elementSizeBytes);
        }
    }

    return paContinue;
}
