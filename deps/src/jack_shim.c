#include <jack/jack.h>
#include <pa_ringbuffer.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <assert.h>

#define MIN2(x, y) ((x) < (y) ? (x) : (y))
#define MIN3(x, y, z) (MIN2(MIN2(x,y), z))
#define JACK_SHIM_MAX_PORTS (64)

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
    jack_port_t *inports[JACK_SHIM_MAX_PORTS];
    jack_port_t *outports[JACK_SHIM_MAX_PORTS];
    PaUtilRingBuffer *inputbufs[JACK_SHIM_MAX_PORTS]; // ringbuffer for input
    PaUtilRingBuffer *outputbufs[JACK_SHIM_MAX_PORTS]; // ringbuffer for output
    PaUtilRingBuffer *errorbuf; // ringbuffer to send error notifications
    int sync; // keep input/output ring buffers synchronized (0/1)
    int inputchans;
    int outputchans;
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
 * This routine will be called by the Jack engine when audio is needed.
 * It may called at interrupt level on some machines so don't do anything that
 * could mess up the system like calling malloc() or free().
 */
// int jack_shim_processcb(const void *input, void *output,
//                      unsigned long frameCount,
//                      const PaStreamCallbackTimeInfo* timeInfo,
//                      PaStreamCallbackFlags statusFlags,
//                      void *userData)
int jack_shim_processcb(unsigned long frameCount, void *userData)
{
    jack_shim_info_t *info = userData;

    // PaUtilRingBuffer *inputbuf; // ringbuffer for input
    // PaUtilRingBuffer *outputbuf; // ringbuffer for output
    // PaUtilRingBuffer *errorbuf; // ringbuffer to send error notifications

    if(info->notifycb == NULL) {
        fprintf(stderr, "jack_shim ERROR: notifycb is NULL\n");
    }

    assert(info->inputchans <= JACK_SHIM_MAX_PORTS);
    int inch, nwrite, nwrite_sync=INT_MAX;
    for(inch=0; inch<info->inputchans; inch++) {
        if(info->inputbufs[inch]) {
            nwrite = PaUtil_GetRingBufferWriteAvailable(info->inputbufs[inch]);
            nwrite = MIN3(frameCount, nwrite, nwrite_sync);
            if(info->sync) {
                nwrite_sync = nwrite;
            }
        }
    }

    assert(info->outputchans <= JACK_SHIM_MAX_PORTS);
    int outch, nread, nread_sync=INT_MAX;
    for(outch=0; outch<info->inputchans; outch++) {
        if(info->outputbufs[outch]) {
            nread = PaUtil_GetRingBufferReadAvailable(info->outputbufs[outch]);
            nread = MIN3(frameCount, nread, nread_sync);
            if(info->sync) {
                nread_sync = nread;
            }
        }
    }

    // if(info->inputbuf && info->outputbuf && info->sync)
    // So, info->sync really means sync all input channels with all output channels
    if(nwrite > 0 && nread > 0 && info-sync) {
        // to keep the buffers synchronized, set readable and writable to
        // their minimum value
        nread = MIN2(nread, nwrite);
        nwrite = nread;
    }

    // read/write from the ringbuffers
    for(inch=0; inch<info->inputchans; inch++) {
        if(info->inputbufs[inch]) {
            PaUtil_WriteRingBuffer(info->inputbufs[inch], input, nwrite);
            if(info->notifycb) {
                info->notifycb(info->inputhandle); // should we notify inputchans times?
            }
            if(nwrite < frameCount) {
                senderr(info, JACK_SHIM_ERRMSG_OVERFLOW);
            }
        }
    }

    for(outch=0; outch<info->outputchans; outch++) {
        if(info->outputbufs[outch]) {
            PaUtil_ReadRingBuffer(info->outputbufs[outch], output, nread);
            if(info->notifycb) {
                info->notifycb(info->outputhandle); // should we notify outputchans times?
            }
            if(nread < frameCount) {
                // the below line will send outputchans err messages - is this too spammy?
                senderr(info, JACK_SHIM_ERRMSG_UNDERFLOW);
                // we didn't fill the whole output buffer, so zero it out
                memset(output+nread*info->outputbufs[outch]->elementSizeBytes, 0,
                    (frameCount - nread)*info->outputbufs[outch]->elementSizeBytes);
            }
        }
    }

    return 0;
}
