/*  quadEM.cc

    Author: Mark Rivers
    Date: April 10, 2003, based on ip330.cc

*/

#include <vxWorks.h>
#include <vxLib.h>
#include <sysLib.h>
#include <vme.h>
#include <math.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include <tickLib.h>

#include "quadEM.h"
#include "ipUnidig.h"
#define MAX_A24_ADDRESS  0xffffff

/* First word of every command */
#define COMMAND1        0xa000 
/* Other commands */
#define RANGE_COMMAND   1
#define GO_COMMAND      4
#define CONV_COMMAND    5
#define PULSE_COMMAND   6
#define PERIOD_COMMAND  7

extern "C"
{
#ifdef NODEBUG
#define DEBUG(l,f,v) ;
#else
#define DEBUG(l,f,v...) { if(l<quadEMDebug) printf(f,## v); }
#endif
int quadEMDebug = 0;
}

extern "C" quadEM *initQuadEM(unsigned short *baseAddr, int fiberChannel, 
                              int microSecondsPerScan, int maxClients, 
                              IpUnidig *pIpUnidig, int unidigChan)
{
    quadEM *pQuadEM = new quadEM(baseAddr, fiberChannel, microSecondsPerScan,
                                 maxClients, pIpUnidig, unidigChan);
    return pQuadEM;
}

quadEM::quadEM(unsigned short *baseAddr, int fiberChannel, int microSecondsPerScan,
               int maxClients, IpUnidig *pIpUnidig, int unidigChan)
: pIpUnidig(pIpUnidig), maxClients(maxClients), numClients(0)
{
    client = (quadEMClient *)calloc(maxClients, sizeof(quadEMClient));
    if (fppProbe()==OK) 
       pFpContext = (FP_CONTEXT *)calloc(1, sizeof(FP_CONTEXT));
    else
       pFpContext = NULL;

    if((fiberChannel >= 4) || (fiberChannel < 0)) {
        printf("quadEM::quadEM: Invalid channel # %d \n", fiberChannel);
        return;
    }

    if(baseAddr >= (unsigned short *)MAX_A24_ADDRESS) {
        printf("quadEM::quadEM: Invalid Module Address %p \n", baseAddr);
        return;
    }

    /* The channel # goes in bits 5 and 6 */
    baseAddr = (unsigned short *)((int)baseAddr | ((fiberChannel << 5) & 0x60));
    if(sysBusToLocalAdrs(VME_AM_STD_USR_DATA, (char *)baseAddr,
                         (char **)&baseAddress) != OK) {
        baseAddress = NULL;
        printf("quadEM::quadEM: A24 Address map failed\n");
        return;
    }

    unsigned long probeVal;
    if (vxMemProbe((char *)baseAddress, READ, 4, (char *)&probeVal) != OK ) {
        printf("quadEM::quadEM: vxMemProbe failed for address %p", baseAddress);
        baseAddress = NULL;
        return;
    }

    if (pIpUnidig == NULL) {
        if (taskSpawn("quadEMPoller",100,VX_FP_TASK,4000,
            (FUNCPTR)quadEM::poller,int(this),0,0,0,0,0,0,0,0,0) == ERROR)
            printf("quadEM poller taskSpawn Failure\n");
    }
    else {
        int mask = 1<<(unidigChan-1);
        pIpUnidig->setFallingMaskBits(mask);
        pIpUnidig->registerCallback(intFunc, (void *)this, mask);
    }
    setMicroSecondsPerScan(microSecondsPerScan);
}

void quadEM::poller(quadEM *p)
//  This functions runs as a polling task at 60Hz if there is no 
//  IP-Unidig present
{
    while(1) { // Do forever
        intFunc(p, 0);
        taskDelay(1);
    }
}

quadEMData quadEM::getData()
{
    return(data);
}

int quadEM::registerCallback(quadEMCallback callback, void *pvt)
{
    if (numClients >= maxClients) {
       printf("quadEM::registerClient: too many clients\n");
       return(-1);
    }
    numClients = numClients + 1;
    client[numClients-1].pvt = pvt;
    client[numClients-1].callback = callback;
    return(0);
}
       
void quadEM::intFunc(void *v, unsigned int mask)
{
    quadEM *t = (quadEM *) v;
    int    i;

    // Save and restore FP registers so application interrupt functions can do
    // floating point operations.  Skip if no fpp hardware present.
    if (t->pFpContext != NULL) fppSave (t->pFpContext);
    // Read the new data
    t->read();
    t->data.computePosition();
    // Call the callback routines which have registered
    for (i = 0; i < t->numClients; i++) {
       t->client[i].callback(t->client[i].pvt, t->data);
    }
    if (t->pFpContext != NULL) fppRestore(t->pFpContext);
}

float quadEM::setMicroSecondsPerScan(float microSeconds)
{
    /* Convert from microseconds to device units */
    int convValue = (int)((microSeconds - 0.6)/1.6);
    write(CONV_COMMAND, convValue);
    // If we are using the ipUnidig then this is the scan rate
    // except that we only get interrupts after every other cycle
    // because of ping/pong, so we multiply by 2.
    if (pIpUnidig != NULL) {
        actualMicroSecondsPerScan = 2. * convValue * 1.6 + 0.6;
    } else {
        actualMicroSecondsPerScan = 1.e6 * 1./sysClkRateGet();
    }
    return(getMicroSecondsPerScan());
}

// Note: we cache actualMicroSecondsPerScan for efficiency.
// It is important for getMicroSecondsPerScan to be efficient, since
// it is often called from the interrupt routines of servers which use quadEM.
float quadEM::getMicroSecondsPerScan()
{
    return(actualMicroSecondsPerScan);
}

void quadEM::setOffset(int channel, int value)
{
    data.offset[channel] = value;
}

void quadEM::setGain(int value)
{
    write(RANGE_COMMAND, value);
}

void quadEM::setPeriod(int period)
{
    // For now we use a fixed period
    write(PERIOD_COMMAND, 0xffff);
}

void quadEM::go()
{
    write(GO_COMMAND, 1);
}

void quadEM::setPulse(int value)
{
    write(PULSE_COMMAND, value);
}

void quadEM::read()
{
   data.current[0] = *(baseAddress+0)  - data.offset[0];
   data.current[1] = *(baseAddress+4)  - data.offset[1];
   // Note that the following seems strange, but diode 4 comes before diode 3 in
   // memory.
   data.current[2] = *(baseAddress+12)  - data.offset[2];
   data.current[3] = *(baseAddress+8) - data.offset[3];
}

void quadEM::write(int command, int value)
{
   *(baseAddress+12) = (unsigned short)COMMAND1;
   *(baseAddress+8)  = (unsigned short)command;
   *(baseAddress+4)  = (unsigned short)value;
   *(baseAddress+16) = (unsigned short)0;
   DEBUG(5, "quadEM::write, command=%d value=%d\n", command, value);
}


// These are the class definitions for the quadEMData class
// Constructor with no arguments
quadEMData::quadEMData()
{ 
    int i;
    for (i=0; i<4; i++) {
        current[i] = 0;
    }
    for (i=0; i<2; i++) {
        sum[i] = 0;
        difference[i] = 0;
        position[i] = 0;
    }
}

// Constructor with 1 argument
quadEMData::quadEMData(int value)
{ 
    int i;
    for (i=0; i<4; i++) {
        current[i] = value;
    }
    for (i=0; i<2; i++) {
        sum[i] = value;
        difference[i] = value;
        position[i] = value;
    }
}

void quadEMData::computePosition()
{
    int i;
    sum[0]  = current[0] + current[2];
    difference[0] = current[2] - current[0];
    if (sum[0] == 0) sum[0] = 1;
    position[0]  = (QUAD_EM_POS_SCALE * difference[0]) / sum[0];
    sum[1]  = current[1] + current[3];
    difference[1] = current[3] - current[1];
    if (sum[1] == 0) sum[1] = 1;
    position[1]  = (QUAD_EM_POS_SCALE * difference[1]) / sum[1];
    //Copy the data to an int array, since this is needed by many clients
    for (i=0; i<4; i++) array[i] = current[i];
    for (i=0; i<2; i++) {
       array[i+4] = sum[i];
       array[i+6] = difference[i];
       array[i+8] = position[i];
    }
}
