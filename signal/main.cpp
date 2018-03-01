/*
 *  Copyright (C) 2017, CSWales <cwales@medeagames.com>
 
 *  This code takes its inspiration (and a few of its lines) from
 *  inspectrum, a tool for visualizing captured RF spectra. 
 *  inspectrum is copyright 2015, Mike Walters <mike@flomp.net> under GPL3
 *
*
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <unistd.h>
#include <math.h>
#include <fftw3.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <float.h>


char *executableName;
#define FFT_SIZE 128
#define FFT_PER_CHUNK 1000

#ifndef FALSE
#define FALSE 0
#endif

#ifndef TRUE
#define TRUE 1
#endif

#ifndef MAX
#define MAX(a,b) (a>b?a:b)
#endif 

#ifndef MIN
#define MIN(a,b) (a<b?a:b)
#endif

void printUsage()
{
    fprintf(stderr, "Wireless Signal Finder\n");
    fprintf(stderr, "Finds signal chirps in an raw signal input file, outputs array of signal\n");
    fprintf(stderr, "times and durations\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "%s [-r <sampleRate, defaults to 1000000>] [file]\n", executableName);
    fprintf(stderr, "Will use stdin as input if file not specified\n");
    fprintf(stderr, "\n");
}



// Convert complex unsigned char to normalized complex float
static void convertToComplexFloat(unsigned char *src, float *dst, int nSamples)
{
    float *dstPtr = dst;
    unsigned char  *srcPtr = src;
    for (int i=0; i<nSamples; i++) {
        const float k = 1.0f / 128.0f;
        *dstPtr++ = (*srcPtr++ - 127.4f) * k;
        *dstPtr++ = (*srcPtr++ - 127.4f) * k;
    }
}

// debug
float lastPeak;
float lastSNR;

// FFT. Used by Processing

static fftwf_plan fftwfPlan = NULL;
static float *window = NULL;
static int fftSize = 128;
static fftwf_complex *fft_dst = NULL;
static fftwf_complex *fft_src  = NULL;

// XXX  ?? what does this do, anyway? There's a normalization across the buffer.
static void initWindow() 
{
    static const double Tau = M_PI * 2.0;
    window = (float *)malloc(fftSize * sizeof(float)); 
    for (int i = 0; i < fftSize; i++) {
        window[i] = 0.5f * (1.0f - cos(Tau * i / (fftSize - 1)));
    }
}

void initFFT(int fft_Size)
{
    fftSize = fft_Size;
    fft_src = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * fftSize);
    fft_dst = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * fftSize);

    fftwfPlan = fftwf_plan_dft_1d(fftSize, 
                                fft_src, 
                                fft_dst, 
                                FFTW_FORWARD, 
                                FFTW_MEASURE);
                           
    initWindow();
}

void destroyFFT() 
{
    if (fftwfPlan) {
        fftwf_destroy_plan(fftwfPlan);
        fftwfPlan = NULL; 
    }
    if (window != NULL) { 
        free(window);
        window = NULL;
    } 
}

void getFFT(float *src, float *dst) 
{                               
    for (int i = 0; i < fftSize; i++) {
        src[i*2]     *= window[i];
        src[i*2 + 1] *= window[i];
    }
    memcpy(fft_src, src, fftSize*sizeof(fftwf_complex));
    fftwf_execute(fftwfPlan);
    
    const float invFFTSize = 1.0f / fftSize;
    const float logMultiplier = 10.0f / log2f(10.0f);
    int k = fftSize/2; // start from the middle of the FFTW array and wrap, putting signal in the center
    float *dstPtr = dst;

    for (int i = 0; i < fftSize; i++) {
        float real = fft_dst[k][0]*invFFTSize;
        float imag = fft_dst[k][1]*invFFTSize;
        float power = real*real + imag*imag;
        float logPower = log2f(power) * logMultiplier;
        *dstPtr++ = logPower;
        k++;
        if (k >= fftSize) {
            k = 0;
        }
    }
}


// - Processing. 

static int chunkSize = 0;  // Size of chunk, in samples.
static float *dstBuffer = NULL;
static float *sampleBuffer = NULL;
static unsigned long timebase = 0;
static bool bProcessingInit = false;
static unsigned long sampleRate = 1000000;
static unsigned int processingStride;
static unsigned int nChunksRead = 0;
static unsigned long signalStartTime = 0;
static unsigned long bytesProcessed = 0;

//static bool signalDetected = false;

float *firstSynchBuffer     = NULL;
float *secondSynchBuffer    = NULL;

#define THRESHOLD -15.0f
#define NOISE_THRESHOLD -50.0f

typedef enum {
    FIRST_SYNCH,
    TRANSITION_TO_SECOND_SYNCH,
    SECOND_SYNCH,
    TRANSITION_OUT_OF_SYNCH
} eSynchState;

typedef enum {
    NO_MESSAGE,
    SYNCHING,
    IN_MESSAGE
} eProcessingState;

typedef enum {
    MSG_SIGNAL,
    MSG_NO_SIGNAL,
    MSG_TRANSITION = 3,
    MSG_UNKNOWN = 4
} eSignalState;

eSynchState synchState;
eProcessingState processingState;
eSignalState msgState;

#define SYNCH_SETTLE_TIME 30

unsigned long firstSynchStartTime  = 0;
unsigned long secondSynchStartTime = 0;


static void resetProcessingState(void);
static bool findTransmission(float *buffer, int bufferLen, int *peak, float *s2nr);

void processingInit(int fftSize, int rate, int stride)
{
    if (!bProcessingInit) {
        initFFT(fftSize);
        chunkSize = fftSize;
        sampleBuffer = (float *)malloc(chunkSize * 2 * sizeof(float));
        dstBuffer    = (float *)malloc(chunkSize * sizeof(float));
        firstSynchBuffer  = (float *)malloc(chunkSize * sizeof(float));
        secondSynchBuffer = (float *)malloc(chunkSize * sizeof(float));
        timebase = 0;
        sampleRate = rate;
        processingStride = stride;
        resetProcessingState();
    }
    bProcessingInit = true;
}

void processingShutDown()
{
    if (!bProcessingInit) {
        destroyFFT();
        if (sampleBuffer) {
            free(sampleBuffer);
            sampleBuffer = NULL;
        }
        if (dstBuffer) {
            free(dstBuffer);
            dstBuffer = NULL;
        }
        
        if (firstSynchBuffer) {
            free(firstSynchBuffer);
            firstSynchBuffer = NULL;
        }
        
        if (secondSynchBuffer) {
            free(secondSynchBuffer);
            secondSynchBuffer = NULL;
        }
    }
    bProcessingInit = false;
}


static void emitSignal(int startTime, int duration)
{
    fprintf(stdout, "[%d, %d]\n", startTime, duration);
    fflush(stdout); // yeah, the \n should flush it. Don't know wtf is happening
}

float s2nrThreshold = 0.50f;  // XXX nfc what this should be. Check empirically

// XXX - I could find the transmission frequency empirically, just by looking at what it
// is over time. Later.

static bool transmissionPresent(float *buffer, int bufferLen)
{
    // Go through buffer, looking for a signal that rises
    // above the average power
    int peak;
    float s2nr;
    if( findTransmission(buffer, bufferLen, &peak, &s2nr) ){
        //fprintf(stderr, "Peak at %d\n", peak); // XXX DEBUG only
        return (s2nr < s2nrThreshold); // since power is negative, the snr threshold points this way
    } else {
        return false;
    }
}

/*
With this version, I'm just comparing total power. This is not very useful for comparing
different spectra with similar power, or for comparing random noise with random noise 
*/
/*
static float signalDifferential(float *buffer1, float *buffer2, int bufferLen) 
{
    //float diffBuf[bufferLen]; 
    //float *diffBufPtr = diffBuf;
    float *buffer1Ptr = buffer1;
    float *buffer2Ptr = buffer2;
    float cumDif = 0;
    
    for (int i=0; i<bufferLen; i++) {
        float diff = *buffer1Ptr++ - *buffer2Ptr++;  // intentionally signed. random errors should cancel.
   //     *diffBufPtr++ = diff;
        cumDif += diff;
    }
    // For the moment, I'll just use the cumulative differential
    printf("Signal Differential is %f\n", cumDif/bufferLen);
    return cumDif/bufferLen;
}
*/


// Slightly different version - compare snr for the two buffers
static float signalDifferential(float *buffer1, float *buffer2, int bufferLen) 
{
    int peak1, peak2;
    float snr1, snr2;
    bool hasTransmission1, hasTransmission2;
    float retVal = 1.0f;
    
    hasTransmission1 = findTransmission(buffer1, bufferLen, &peak1, &snr1);
    hasTransmission2 = findTransmission(buffer2, bufferLen, &peak2, &snr2);
        
    // signal differential is snr/snr, for with smaller snr as numerator, larger
    // as denominator, if we have transmissions for both
    if (hasTransmission1 && hasTransmission2) {
        float snrMax = MAX(snr1,snr2);
        float snrMin = MIN(snr1,snr2);
        retVal = snrMin/snrMax;
    // If there is only a transmission for one of them, return 0.1
    } else if (hasTransmission1 || hasTransmission2) {
        retVal = 0.1;
    // If there is a transmission for neither, return 1.0
    } else {
       retVal = 1.0;
    } 
    
//    printf("Signature differential is %f\n", retVal);
    return retVal;
}



//bool bInTransmission = false;

unsigned long lastTransmissionTime = 0;

float *signalSignature      = NULL;
float *spaceSignature       = NULL;

#define INITIAL_SPACE_MIN 30000
#define INITIAL_SPACE_MAX 60000
#define INITIAL_SIGNAL_MIN 800
#define INITIAL_SIGNAL_MAX 1200

//#define ID_THRESHOLD 35.f // XXX I have no idea how big this should be
#define ID_THRESHOLD 0.8f // XXX I have no idea how big this should be

#define END_MSG_TIMEOUT 1000 // 1 millisecond without a signal is considered to be the end of a transmission

static int identifyBuffer(float *buffer, int bufferLen)
{
    if (bufferLen != fftSize) {
        fprintf(stderr, "Unexpected buffer size!\n");
        return MSG_UNKNOWN;
    }
    // Attempt to figure out what this buffer represents - signal, space between signals, 
    // or a transitional state. Note that I'm changing the order in which I do the comparisons
    // depending on the current state, since the buffer will almost always be the same
    // type as the previous one.
    if (msgState == MSG_SIGNAL) {
        if (signalSignature && signalDifferential(buffer, signalSignature, bufferLen) > ID_THRESHOLD) {
            return MSG_SIGNAL;
//} else if (transitionSignature && signalDifferential(buffer, transitionSignature, bufferLen) > ID_THRESHOLD) {
//            return MSG_TRANSITION;
        } else if (spaceSignature && signalDifferential(buffer, spaceSignature, bufferLen) > ID_THRESHOLD) {
            return MSG_NO_SIGNAL;
        } else {
            return MSG_UNKNOWN;
        }
    } else if (msgState == MSG_NO_SIGNAL) {
        if (spaceSignature && signalDifferential(buffer, spaceSignature, bufferLen) > ID_THRESHOLD) {
            return MSG_NO_SIGNAL;
//        } else if (transitionSignature && signalDifferential(buffer, transitionSignature, bufferLen) > ID_THRESHOLD) {
//            return MSG_TRANSITION;
        } else if (signalSignature && signalDifferential(buffer, signalSignature, bufferLen) > ID_THRESHOLD) {
            return MSG_SIGNAL;
        } else {
            return MSG_UNKNOWN;
        }
    } else if (msgState == MSG_TRANSITION || msgState == MSG_UNKNOWN) {
        if (signalSignature && signalDifferential(buffer, signalSignature, bufferLen) > ID_THRESHOLD) {
            return MSG_SIGNAL;
//        } else if (transitionSignature && signalDifferential(buffer, transitionSignature, bufferLen) > ID_THRESHOLD) {
//            return MSG_TRANSITION;
        } else if (spaceSignature && signalDifferential(buffer, spaceSignature, bufferLen) > ID_THRESHOLD) {
            return MSG_NO_SIGNAL;
        } else {
            return MSG_UNKNOWN;
        }
    }
    
    return MSG_UNKNOWN;
}

/* processBuffer -
   Take an incoming buffer with data in the frequency domain, and call emitSignal if 
   needed. In order to emit a signal, we must know both the start of the signal and the 
   duration of the signal, so we need to track state transitions between signalling and 
   non-signalling.
   
   The process of tracking transitions is somewhat complicated because empirically, the 
   spectrum received appears to have four distinct states*, and not all of them 
   necessarily appear in each data packet.
   
   To add to the complexity, although the signatures of these states are consistent 
   across a single data packet transmission, they can vary wildly depending on the 
   particular transmitter, radio, and distance between the two. The same equipment in the 
   same geometric configuration can even show differences in received transmission 
   signatures for packets sent a few seconds apart. 
   
   To deal with all of this, I introduce a fairly complex state machine and incorporate
   some logic specific to the GE Wireless sensors.**  In particular, a few milliseconds 
   before the beginning of the data bits, the radio puts out a signature equivalent to the
   'space' between actual signals. (Depending on the distance between the radio and the 
   transmitter, however, this transmission may or may not be distinguishable from the 
   background noise.) After this signature there is always an approximately 1ms 
   transmission in the 'signalled' state. Therefor, in order to figure out what is signal 
   and what is space, I look at the first two transmissions in the packet. Whichever 
   transmission is about 1ms long is signal, the other is space.
   
   * Signalled, non-signalled, transitional, and background. There's always a visible
   frequency spike in the signalled state. Non-signalled and transitional states may
   look like background noise.
   
   ** I'm sad about having to bring this high-level knowledge of the protocol down into
     this low-level routine. The core problem is that I can't know whether the start of
     the transmission represents signal or space. Perhaps there's a better way than 
     matching the initial data to what I expect to see from the GE Sensors - maybe, for
     instance, the transmission is always significantly stronger during a signal than 
     during a space.   
*/


static void resetProcessingState()
{
    synchState = FIRST_SYNCH;
    processingState = NO_MESSAGE;
    msgState = MSG_NO_SIGNAL;
}

/* GE_DifferentiateSignalFromSpace
   GE-specific code here. At this point we should have the first two 
   transitions in a packet, and we should be able to figure out which
   is signal and which is space. In this routine, we do this by looking
   at the transmission lengths, which are specific to GE Wireless sensors
   
   Note that we could create other similar functions for other sensors,
   or we could replace this function with something more general */
static bool GE_DifferentiateSignalFromSpace(int firstSynchDuration, int secondSynchDuration)
{
    bool startBeforeSignal;
    
    //printf("Diff signal from space, firstDuration %d, secondDuration %d\n", firstSynchDuration, secondSynchDuration);
    
    // does the second buffer look more like the longish signal blip,
    // or more like the short no-signal blip that follows the longish
    // signal blip?
    if (secondSynchDuration <= INITIAL_SIGNAL_MAX && 
        secondSynchDuration >= INITIAL_SIGNAL_MIN) {
        startBeforeSignal = true;
    } else if (firstSynchDuration <= INITIAL_SIGNAL_MAX && 
               firstSynchDuration >= INITIAL_SIGNAL_MIN) {
        startBeforeSignal = false;
    } else {
        fprintf(stderr, "GE packet error - %d, %d\n", firstSynchDuration, secondSynchDuration);
        return false; // doesn't look like a GE Wireless packet
    }
    
    if (startBeforeSignal) {
        spaceSignature  = firstSynchBuffer;
        signalSignature = secondSynchBuffer;        
    } else {
        spaceSignature  = secondSynchBuffer;
        signalSignature = firstSynchBuffer; 
    }  
    
    return true;     
}

unsigned long prevStartTime = 0;
unsigned long prevTime = 0;

static void processBuffer(float *buffer, int bufferLen)
{
    bool transmitting = transmissionPresent(buffer, bufferLen);
    int signalType; 
    unsigned long curTime;
 
    bytesProcessed += processingStride; 
    curTime = bytesProcessed/(((float)sampleRate)/1000000);
    if (curTime < prevTime) printf("WRAP\n");
    prevTime = curTime;   
 
    // make note of the last time we saw a transmission. This is used to change the 
    // state at the end of a message
    if (transmitting) {
        lastTransmissionTime = curTime;
    }
    
//    fprintf(stderr, "Processing state is %d,transmitting %d, synchState %d, curtime %d\n", processingState, transmitting, synchState, curTime);
    
    switch (processingState) {
    case NO_MESSAGE:
        if (!transmitting) {
            // reset timebase if it's been more than 5 seconds since the previous signal
            if (time(0) - timebase > 5) {
                timebase = time(0);
                printf("TIMEBASE RESET, time is %lu\n", timebase);
                bytesProcessed = 0;
            }
        } else {
            //fprintf(stderr, "Detected message, %lu\n", curTime);
            // Beginning of a message. Start the synching process.
            firstSynchStartTime = curTime;
            processingState = SYNCHING;
            synchState = FIRST_SYNCH;
            if (curTime < prevStartTime) printf("CSW WRAP!\n");
            prevStartTime = curTime;
            fprintf(stderr, "Start of message - Found signal, time %lu, peak %f, snr %f\n", curTime, lastPeak, lastSNR);
        }
        break;
    case IN_MESSAGE:
        // is this space or signal? 
        if (!transmitting) {
            signalType = MSG_NO_SIGNAL;
            //fprintf(stderr, "no signal\n");
        } else {
            signalType = identifyBuffer(buffer, bufferLen);
            //fprintf(stderr, "Signal type is %d\n", signalType);
        } 
        
        //fprintf(stderr, "Diff time is %lu\n", curTime - lastTransmissionTime);
        //fprintf(stderr, "cur time %lu, last trans time %lu\n", curTime, lastTransmissionTime);
        
        // quick check - has the message ended? If so, change state and immediately break
        if ((signalType == MSG_NO_SIGNAL || signalType == MSG_UNKNOWN) && 
            (curTime - lastTransmissionTime > END_MSG_TIMEOUT)) {
            fprintf(stderr, "END MESSAGE, time %lu\n", curTime);
            resetProcessingState(); 
            break;
        } 
        
        
        // normal flow - emit signal on state change, otherwise nop
        switch (msgState){
        case MSG_SIGNAL:
            if (signalType == MSG_NO_SIGNAL) {
                // state changes. Emit current signal.
                msgState = MSG_NO_SIGNAL;
                emitSignal(signalStartTime, curTime - signalStartTime);
            } else {
                // nop. Treat 'unknown' and 'transition' as part of the signal.
            }
            break;
        case MSG_NO_SIGNAL:
            if (signalType == MSG_SIGNAL) {
                fprintf(stderr, "Found signal, time %lu, peak %f, snr %f\n", curTime, lastPeak, lastSNR);
                // state changes. Set signal start time
                msgState = MSG_SIGNAL;
                signalStartTime = curTime;
            } else {
                // nop. Treat 'unknown' and 'transition' as part of the space
            } 
            break;
        default:
            break;
        }
        break;
    case SYNCHING:
        switch (synchState) {
        case FIRST_SYNCH:
            if (curTime - firstSynchStartTime >= SYNCH_SETTLE_TIME) {
                if (transmitting) {
                    memcpy(firstSynchBuffer, buffer, bufferLen*sizeof(float));
                    synchState = TRANSITION_TO_SECOND_SYNCH;    
                    fprintf(stderr, "FIRST SYNCH %lu\n",curTime);                
                    fprintf(stderr, "Sync signal, time %lu, peak %f, snr %f\n", curTime, lastPeak, lastSNR);
                } else {
                    //fprintf(stderr, "signal inconsistency in first sync\n"); // XXX - it may be better to just ignore this, or have it be only a special debug printf.
                }
            } else {
                // nop. Settling after transition to first sync.
            }
            break;
        case TRANSITION_TO_SECOND_SYNCH:
            if (!transmitting || 
                signalDifferential(firstSynchBuffer, buffer, bufferLen) < ID_THRESHOLD) { // XXX may want the threshold bigger here?
                    synchState = SECOND_SYNCH;
                    fprintf(stderr, "SECOND SYNCH %lu\n", curTime);
                    secondSynchStartTime = curTime;
            } else {
                // nop. Haven't found the transition point yet.
            }
            break;
        case SECOND_SYNCH:
            if (curTime - secondSynchStartTime >= SYNCH_SETTLE_TIME) {
                memcpy(secondSynchBuffer, buffer, bufferLen*sizeof(float));
                synchState = TRANSITION_OUT_OF_SYNCH;
                fprintf(stderr, "Sync signal, time %lu, peak %f, snr %f\n", curTime, lastPeak, lastSNR);
            } else {
                // nop. Settling after transition to second sync
            }
            break;
        case TRANSITION_OUT_OF_SYNCH:
            if (signalDifferential(secondSynchBuffer, buffer, bufferLen) < ID_THRESHOLD) { // XXX may want the threshold bigger here?
                fprintf(stderr, "CHECK SYNCHS %lu\n", curTime);
                if (GE_DifferentiateSignalFromSpace(secondSynchStartTime - firstSynchStartTime, 
                                                     curTime - secondSynchStartTime)){
                    if (firstSynchBuffer == spaceSignature) {
                        emitSignal(secondSynchStartTime, curTime - secondSynchStartTime);
                        msgState = MSG_NO_SIGNAL;
                    } else {
                        emitSignal(firstSynchStartTime, secondSynchStartTime - firstSynchStartTime);
                        msgState = MSG_SIGNAL;
                        signalStartTime = curTime;
                    } 
                    processingState = IN_MESSAGE;
                } else {
                    fprintf(stderr, "Not GE packet. Ignoring\n");
                    resetProcessingState();
                }
            } else {
                // nop. Haven't found the transition point yet.
            }
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }
}

#define SLIDING_WINDOW_SIZE 2  // XXX check experimentally
static bool findTransmission(float *buffer, int bufferLen, int *peak, float *s2nr)
{
    //float slidingWindow[SLIDING_WINDOW_SIZE]; // XXX this is a better optimization... Do I need it?
    float *bufferPtr = buffer;
    float maxWindowPower = -FLT_MAX;   // maximum power in any particular window
    float accPower = 0;   // total power in the spectrum
    int transmissionFreqStart = 0;
    
    if (bufferLen <= SLIDING_WINDOW_SIZE){
        return false;
    }
    
    // go through all the sliding windows, looking for the one that has the highest
    // total power. Also sum the power in the buffer
    for (int j=0; j<bufferLen-SLIDING_WINDOW_SIZE; j++){
        bufferPtr = &buffer[j];
        accPower += *bufferPtr;
        
        float windowPower = 0; 
        for (int i=0; i<SLIDING_WINDOW_SIZE; i++){
            windowPower += *bufferPtr++; 
        }
        if (windowPower > maxWindowPower) {
            maxWindowPower = windowPower;
            transmissionFreqStart = j;
        }
    }
    
    // finish summing the power in the buffer for the last SLIDING_WINDOW_SIZE samples
    bufferPtr = &buffer[bufferLen - SLIDING_WINDOW_SIZE];
    for (int i=0; i<SLIDING_WINDOW_SIZE; i++) {
        accPower += *bufferPtr++;
    }
    
    // When calculating the average power, I want the average *outside* of the peak window.
    // So subtract that from accPower
    accPower -= maxWindowPower;
    
    float averagePower = accPower/(bufferLen - SLIDING_WINDOW_SIZE);
    float peakPower    = maxWindowPower/SLIDING_WINDOW_SIZE;
    
    // for debug at least - find and print peak in window...
/*    float localMaxima = -FLT_MAX;
    bufferPtr = &buffer[transmissionFreqStart];
    for (int i=0; i<SLIDING_WINDOW_SIZE; i++) {
        localMaxima = MAX(localMaxima, *bufferPtr++);
    }*/
//    fprintf(stderr, "DEBUG - window maxima is %f, window power is %f\n", localMaxima, peakPower);
//    fprintf(stderr, "DEBUG - accPower is %f\n", accPower);
//    fprintf(stderr, "DEBUG - snr is %f, average power is %f\n", peakPower/averagePower, averagePower);

    
    if (s2nr) {
        *s2nr = peakPower/averagePower;
    }
    if (peak) {
        *peak = transmissionFreqStart + SLIDING_WINDOW_SIZE/2;
    }
    
    // XXX DEBUG
    lastPeak = *peak;
    lastSNR = *s2nr;
    return true;
}

#if 0
static bool signalPresent(float *buffer, int bufferLen) 
{
    float runningTotal = 0;
    int nNonSignalPoints = 0;
    float max = *buffer;
    float min = max;
    float *bufferPtr = buffer;
    // walk through buffer, looking for value that exceeds THRESHOLD
    for (int i=0; i<bufferLen; i++) {
        float curVal = *bufferPtr++;
        if (curVal < THRESHOLD) {
            nNonSignalPoints++;
            runningTotal += curVal;
        }
        if (curVal  > max) {
            max = curVal;
        }
        if (curVal < min) {
            min = curVal;
        }
    }
/*    if (max > THRESHOLD) {
        fprintf(stderr, "Average is %f\n", runningTotal/nNonSignalPoints);
        fprintf(stderr, "Max is %f, min is %f\n", max, min);
    }
*/
    return ((max > THRESHOLD) && nNonSignalPoints > 0 && runningTotal/nNonSignalPoints < NOISE_THRESHOLD);
}
#endif //0


void processChunk(unsigned char *charBuffer)
{
    if (!bProcessingInit) {
        fprintf(stderr, "Processing not initialized\n");
        return;
    }
    
    convertToComplexFloat(charBuffer, sampleBuffer, chunkSize);
    getFFT(sampleBuffer, dstBuffer);
    processBuffer(dstBuffer, FFT_SIZE);
    /*
    int bHaveSignal = signalPresent(dstBuffer, chunkSize);
    if (bHaveSignal && !signalDetected) {
        signalDetected  = true;
        // reset timebase if it's been more than 5 seconds since the previous signal
        if (time(0) - timebase > 5) {
            timebase = time(0);
            nChunksRead = 0;
        }

        signalStartTime = (nChunksRead*FFT_SIZE/8)/(((float)sampleRate)/1000000); 
    } else if (!bHaveSignal && signalDetected) {
        signalDetected  = false;
        unsigned long signalStopTime  = (nChunksRead*FFT_SIZE/8)/(((float)sampleRate)/1000000);
        emitSignal(signalStartTime, signalStopTime - signalStartTime);
    }
    nChunksRead++;
    */
    
}



int main(int argc, char *argv[])
{
    executableName = argv[0];
    
    int rate = 1000000;
    char *file = NULL;
    int fileno = STDIN_FILENO;
    int bufferSize;
    unsigned char *inputBuf;
    unsigned char *inputPtr;
    unsigned char *readPtr;
    unsigned char *writePtr;
    int stride = FFT_SIZE/8;
    int maxReadSize = FFT_SIZE;
    int nBytesRead;
    int debugCounter = 0;
 
    int c;
    opterr = 0;
    while ((c = getopt (argc, argv, "r:")) != -1) {
        switch (c)
        {
            case 'r':
                rate = atoi(optarg);
                break;
            case '?':
                if (optopt == 'r'){
                    fprintf (stderr, "Option -%c requires an argument.\n", optopt);
                    goto ErrExit;
                } else if (isprint (optopt)) {
                    fprintf (stderr, "Unknown option `-%c'.\n", optopt);
                    goto ErrExit;
                } else {
                    fprintf (stderr, "Unknown option character `\\x%x'.\n", optopt);
                    goto ErrExit;
                }
                break;
            default:
                goto ErrExit;
        }
    }
      
    if (optind < argc) {
        file = argv[optind];
    }
    
    if (file != NULL) {
        fileno = open(file, O_RDONLY);
        if (fileno <= 0) {
            goto ErrExit;
        }
    } 
    
    if (rate <= 0){ 
        goto ErrExit;
    }

    processingInit(FFT_SIZE, rate, stride);

    printf("Sample rate %d, stride %d\n", rate, stride);
    // read from file

    // We need to deal with a sliding window with FFT_SIZE samples. I'm going to handle
    // this by allocating a buffer three times the window size. When we've finished with
    // all of the data in the first window, we copy the second and third windows up one,
    // and reset the pointers.
    bufferSize = FFT_SIZE*2*3; // NB - sample is complex, so two bytes
    inputBuf = (unsigned char *)malloc(bufferSize);
    inputPtr = inputBuf;
    readPtr  = inputBuf;
    maxReadSize = FFT_SIZE;
    while((nBytesRead = read(fileno, inputPtr, maxReadSize)) > 0) {
        debugCounter++;
        inputPtr += nBytesRead;
        while (inputPtr - readPtr >= FFT_SIZE*2) {  // as long as there are at least FFT_SIZE samples to process
            processChunk(readPtr);
            readPtr += stride*2;
            if (readPtr - inputBuf > FFT_SIZE*2) {  // we're finished with FFT_SIZE samples. Copy the other two windows over
                memcpy(inputBuf, inputBuf + FFT_SIZE*2, FFT_SIZE*2*2);
                readPtr  -= FFT_SIZE*2;
                inputPtr -= FFT_SIZE*2;
            }
        }
        maxReadSize = MIN(FFT_SIZE, (inputBuf + bufferSize) - inputPtr);
/*        if (debugCounter > 100) {
            break;
        }
*/

    }
    
    /*
    while((nBytesRead = read(fileno, bufPtr, FFT_SIZE)) > 0) {
        bufPtr += nBytesRead;
        if (bufPtr - inputBuf >= bufferSize){
            processChunk(inputBuf);
            bufPtr = inputBuf;
        }
    }
    */
    
    close(fileno);
    processingShutDown();
    
    return 0;  
    
ErrExit:
    printUsage();
    exit(-1);
}


