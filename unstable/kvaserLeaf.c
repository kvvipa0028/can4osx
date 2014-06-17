//
//  kvaserLeaf.c
//  can4osx_cmd
//
//
// Copyright (c) 2014 Alexander Philipp. All rights reserved.
//
//
// License: GPLv2
//
// ===============================================================================
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation version 2
// of the license.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//
// ===============================================================================
//
// Disclaimer:     IMPORTANT: THE SOFTWARE IS PROVIDED ON AN "AS IS" BASIS. THE AUTHOR MAKES NO
// WARRANTIES, EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION THE IMPLIED
// WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE, REGARDING THE SOFTWARE OR ITS USE AND OPERATION ALONE OR IN
// COMBINATION WITH YOUR PRODUCTS.
//
// IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, INDIRECT, INCIDENTAL OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
//                       GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// ARISING IN ANY WAY OUT OF THE USE, REPRODUCTION, MODIFICATION AND/OR DISTRIBUTION
// OF SOFTWARE, HOWEVER CAUSED AND WHETHER UNDER THEORY OF CONTRACT, TORT
// (INCLUDING NEGLIGENCE), STRICT LIABILITY OR OTHERWISE, EVEN IF THE AUTHOR HAS
// BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// ===============================================================================
//


#include <stdio.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>


/* can4osx */
#include "can4osx.h"
#include "can4osx_debug.h"
#include "can4osx_internal.h"

/* Leaf functions */
#include "kvaserLeaf.h"




static LeafCommandMsgBuf* LeafCreateCommandBuffer( UInt32 bufferSize );
static void LeafReleaseCommandBuffer( LeafCommandMsgBuf* bufferRef );
static UInt8 LeafWriteCommandBuffer(LeafCommandMsgBuf* bufferRef, leafCmd newCommand);
static UInt8 LeafReadCommandBuffer(LeafCommandMsgBuf* bufferRef, leafCmd* readCommand);

static void LeafBulkWriteCompletion(void *refCon, IOReturn result, void *arg0);
static IOReturn LeafWriteToBulkPipe(Can4osxUsbDeviceHandleEntry *self);
static UInt16 LeafFillBulkPipeBuffer(LeafCommandMsgBuf* bufferRef, UInt8 *pipe, UInt16 maxPipeSize);

static canStatus LeafCanWrite (const CanHandle hnd,UInt32 id, void *msg, UInt16 dlc, UInt16 flag);

//Hardware interface function
canStatus LeafInitHardware(const CanHandle hnd);

Can4osxHwFunctions leafHardwareFunctions = {
    .can4osxhwInitRef = LeafInitHardware,
    .can4osxhwCanWriteRef = LeafCanWrite,
};





canStatus LeafInitHardware(const CanHandle hnd)
{
    Can4osxUsbDeviceHandleEntry *self = &can4osxUsbDeviceHandle[hnd];
    self->privateData = calloc(1,sizeof(LeafPrivateData));
    
    if ( self->privateData != NULL ) {
        LeafPrivateData *priv = (LeafPrivateData *)self->privateData;
        
        priv->cmdBufferRef = LeafCreateCommandBuffer(1000);
        if ( priv->cmdBufferRef == NULL ) {
            free(priv);
            return canERR_NOMEM;
        }
        
    } else {
        return canERR_NOMEM;
    }
    
    
    return canOK;
    
}


static canStatus LeafCanWrite (const CanHandle hnd,UInt32 id, void *msg, UInt16 dlc, UInt16 flag)
{
    Can4osxUsbDeviceHandleEntry *self = &can4osxUsbDeviceHandle[hnd];
    
    if ( self->privateData != NULL ) {
        LeafPrivateData *priv = (LeafPrivateData *)self->privateData;

        leafCmd cmd;
        
        cmd.txCanMessage.cmdLen = sizeof(cmdTxCanMessage);
        
        if ( flag & canMSG_EXT ) {
            // Extended ID
            cmd.txCanMessage.cmdNo = CMD_TX_EXT_MESSAGE;
        
            cmd.txCanMessage.rawMessage[0] = (UInt8)((id >> 24) & 0x1f);
            cmd.txCanMessage.rawMessage[1] = (UInt8)((id >> 18) & 0x3f);
            cmd.txCanMessage.rawMessage[2] = (UInt8)((id >> 14) & 0x0f);
            cmd.txCanMessage.rawMessage[3] = (UInt8)((id >> 6 ) & 0xFF);
            cmd.txCanMessage.rawMessage[4] = (UInt8)((id      ) & 0x3f);
        } else {
            // Standard CAN
            cmd.txCanMessage.cmdNo         = CMD_TX_STD_MESSAGE;
            
            cmd.txCanMessage.rawMessage[0] = (UInt8)((id >>  6) & 0x1F);
            cmd.txCanMessage.rawMessage[1] = (UInt8)((id      ) & 0x3F);
        }

        

        // DLC and DATA
        cmd.txCanMessage.rawMessage[5]   = dlc & 0x0F;
        memcpy(&cmd.txCanMessage.rawMessage[6], msg, 8);
        
        LeafWriteCommandBuffer(priv->cmdBufferRef, cmd);
        
        LeafWriteToBulkPipe(self);
        
        
    } else {
        return canERR_INTERNAL;
    }


    return canOK;
}




// The command buffer function


LeafCommandMsgBuf* LeafCreateCommandBuffer( UInt32 bufferSize )
{
	LeafCommandMsgBuf* bufferRef = malloc(sizeof(LeafCommandMsgBuf));
	if ( bufferRef == NULL ) {
		return NULL;
	}
    
	bufferRef->bufferSize = bufferSize;
	bufferRef->bufferCount = 0;
	bufferRef->bufferFirst = 0;
    
	bufferRef->commandRef = malloc(bufferSize * sizeof(leafCmd));
    
	if ( bufferRef->commandRef == NULL ) {
		free(bufferRef);
		return NULL;
	}
    
    bufferRef->bufferGDCqueueRef = dispatch_queue_create("com.can4osx.leafcommandqueue", 0);
	if ( bufferRef->bufferGDCqueueRef == NULL ) {
		LeafReleaseCommandBuffer(bufferRef);
		return NULL;
	}
    
	return bufferRef;
}


void LeafReleaseCommandBuffer( LeafCommandMsgBuf* bufferRef )
{
	if ( bufferRef != NULL ) {
        if (bufferRef->bufferGDCqueueRef != NULL) {
            dispatch_release(bufferRef->bufferGDCqueueRef);
        }
		free(bufferRef->commandRef);
		free(bufferRef);
	}
}


UInt8 LeafTestFullCommandBuffer(LeafCommandMsgBuf* bufferRef)
{
	if (bufferRef->bufferCount == bufferRef->bufferSize) {
		return 1;
	} else {
		return 0;
	}
}


UInt8 LeafTestEmptyCommandBuffer(LeafCommandMsgBuf* bufferRef)
{
    if ( bufferRef->bufferCount == 0 ) {
        return 1;
    } else {
        return 0;
    }
}


static UInt8 LeafWriteCommandBuffer(LeafCommandMsgBuf* bufferRef, leafCmd newCommand)
{
    __block UInt8 retval = 1;
    
    dispatch_sync(bufferRef->bufferGDCqueueRef, ^{
        if (LeafTestFullCommandBuffer(bufferRef)) {
            retval = 0;
        } else {
            bufferRef->commandRef[(bufferRef->bufferFirst + bufferRef->bufferCount++) % bufferRef->bufferSize] = newCommand;
        }
    });
    
	return retval;
}


static UInt8 LeafReadCommandBuffer(LeafCommandMsgBuf* bufferRef, leafCmd* readCommand)
{
    __block UInt8 retval = 1;
    
    dispatch_sync(bufferRef->bufferGDCqueueRef, ^{
        if (LeafTestEmptyCommandBuffer(bufferRef)) {
            retval = 0;
        } else {
            bufferRef->bufferCount--;
            *readCommand = bufferRef->commandRef[bufferRef->bufferFirst++ % bufferRef->bufferSize];
        }
        
    });
    
	return retval;
    
}



static UInt32 LeafCalculateTimeStamp(UInt16 *timerRef,
                                       unsigned int hires_timer_fq)
{
    unsigned long  ulTemp;
    UInt16 result[3];
    unsigned int   uiDivisor = 10 * hires_timer_fq;
    
    result[0] = timerRef[2] / uiDivisor;
    ulTemp     = (timerRef[2] % uiDivisor) << 16;
    
    if (result[0] > 0) {
    }
    
    result[1] = (unsigned short)((ulTemp + timerRef[1]) / uiDivisor);
    ulTemp     = ((ulTemp + timerRef[1]) % uiDivisor) << 16;
    
    result[2] = (unsigned short)((ulTemp + timerRef[0]) / uiDivisor);
    
    return ((int)result[1] << 16) + result[2];
}




void LeafDecodeCommand(Can4osxUsbDeviceHandleEntry *self, leafCmd *cmd) {
    
    switch (cmd->head.cmdNo) {
            
        case CMD_RX_EXT_MESSAGE:
        case CMD_RX_STD_MESSAGE:
            CAN4OSX_DEBUG_PRINT("CMD_RX_XXX_MESSAGE\n");
            break;
            
        case CMD_LOG_MESSAGE:
        {
            
            UInt32 channel = cmd->logMessage.channel;
            UInt32 ident = cmd->logMessage.ident;
            
            CanEvent event;
            //event.eventTag = V_RECEIVE_MSG;
            event.eventTagData.canMsg.canId = cmd->logMessage.ident;
            event.eventTagData.canMsg.canDlc = cmd->logMessage.dlc;
            //event.eventTimestamp = cmd->logMessage.time;
            // TODO
            event.eventTimestamp = LeafCalculateTimeStamp(cmd->logMessage.time, 16);
            
            memcpy(event.eventTagData.canMsg.canData, cmd->logMessage.data, 8);
            
        
            // This should go to a dispatcher
            CAN4OSX_WriteCanEventBuffer(self->canEventMsgBuff,event);
            if (self->canNotification.notifacionCenter) {
                CFNotificationCenterPostNotification (self->canNotification.notifacionCenter, self->canNotification.notificationString, NULL, NULL, true);
            }
            
            
            CAN4OSX_DEBUG_PRINT("CMD_LOG_MESSAGE Channel: %d Id: %X\n", channel, ident);
            
        }
        break;
            
        case CMD_START_CHIP_RESP:
            CAN4OSX_DEBUG_PRINT("CMD_START_CHIP_RESP\n");
            break;
            
        case CMD_GET_CARD_INFO_RESP:
            CAN4OSX_DEBUG_PRINT("Card Info Response Serial %d\n",cmd->getCardInfoResp.serialNumber);
            
            break;
            
        case CMD_GET_BUSLOAD_RESP:
            CAN4OSX_DEBUG_PRINT("CMD_GET_BUSLOAD_RESP - Ignored\n");
            break;
            
        case CMD_RESET_STATISTICS:
            CAN4OSX_DEBUG_PRINT("CMD_RESET_STATISTICS - Ignored\n");
            break;
            
        case CMD_ERROR_EVENT:
            CAN4OSX_DEBUG_PRINT("CMD_ERROR_EVENT - Ignored\n");
            break;
            
        case CMD_RESET_ERROR_COUNTER:
            CAN4OSX_DEBUG_PRINT("CMD_RESET_ERROR_COUNTER - Ignored\n");
            break;
            
        case CMD_USB_THROTTLE:
            CAN4OSX_DEBUG_PRINT("CMD_USB_THROTTLE - Ignored\n");
            break;
            
        case CMD_TREF_SOFNR:
            CAN4OSX_DEBUG_PRINT("CMD_TREF_SOFNR - Ignored\n");
            break;
            
        case CMD_CHECK_LICENSE_RESP:
            CAN4OSX_DEBUG_PRINT("CMD_CHECK_LICENCE_RESP - Ignore\n");
            break;
            
        case CMD_GET_TRANSCEIVER_INFO_RESP:
            CAN4OSX_DEBUG_PRINT("CMD_GET_TRANSCEIVER_INFO_RESP - Ignore\n");
            break;
            
        case CMD_SELF_TEST_RESP:
            CAN4OSX_DEBUG_PRINT("CMD_SELF_TEST_RESP - Ignore\n");
            break;
            
        case CMD_LED_ACTION_RESP:
            CAN4OSX_DEBUG_PRINT("CMD_LED_ACTION_RESP - Ignore\n");
            break;
            
        case CMD_GET_IO_PORTS_RESP:
            CAN4OSX_DEBUG_PRINT("CMD_GET_IO_PORTS_RESP - Ignore\n");
            break;
            
        case CMD_HEARTBEAT_RESP:
            CAN4OSX_DEBUG_PRINT("CMD_HEARTBEAT_RESP - Ignore\n");
            break;
            
        case CMD_SOFTSYNC_ONOFF:
            CAN4OSX_DEBUG_PRINT("CMD_SOFTSYNC_ONOFF - Ignore\n");
            break;
            
        default:
            CAN4OSX_DEBUG_PRINT("UNKNOWN COMMAND - %d\n", cmd->head.cmdNo);
            break;
    }
    
}

//******************************************************
// Translate from baud macro to bus params
//******************************************************
canStatus LeafCanTranslateBaud (SInt32 *const freq,
                                      unsigned int *const tseg1,
                                      unsigned int *const tseg2,
                                      unsigned int *const sjw,
                                      unsigned int *const nosamp,
                                      unsigned int *const syncMode)
{
    switch (*freq) {
        case canBITRATE_1M:
            *freq     = 1000000L;
            *tseg1    = 4;
            *tseg2    = 3;
            *sjw      = 1;
            *nosamp   = 1;
            *syncMode = 0;
            break;
            
        case canBITRATE_500K:
            *freq     = 500000L;
            *tseg1    = 4;
            *tseg2    = 3;
            *sjw      = 1;
            *nosamp   = 1;
            *syncMode = 0;
            break;
            
        case canBITRATE_250K:
            *freq     = 250000L;
            *tseg1    = 4;
            *tseg2    = 3;
            *sjw      = 1;
            *nosamp   = 1;
            *syncMode = 0;
            break;
            
        case canBITRATE_125K:
            *freq     = 125000L;
            *tseg1    = 10;
            *tseg2    = 5;
            *sjw      = 1;
            *nosamp   = 1;
            *syncMode = 0;
            break;
            
        case canBITRATE_100K:
            *freq     = 100000L;
            *tseg1    = 10;
            *tseg2    = 5;
            *sjw      = 1;
            *nosamp   = 1;
            *syncMode = 0;
            break;
            
        case canBITRATE_83K:
            *freq     = 83333L;
            *tseg1    = 5;
            *tseg2    = 2;
            *sjw      = 2;
            *nosamp   = 1;
            *syncMode = 0;
            break;
            
        case canBITRATE_62K:
            *freq     = 62500L;
            *tseg1    = 10;
            *tseg2    = 5;
            *sjw      = 1;
            *nosamp   = 1;
            *syncMode = 0;
            break;
            
        case canBITRATE_50K:
            *freq     = 50000L;
            *tseg1    = 10;
            *tseg2    = 5;
            *sjw      = 1;
            *nosamp   = 1;
            *syncMode = 0;
            break;
            
        case canBITRATE_10K:
            *freq     = 10000L;
            *tseg1    = 11;
            *tseg2    = 4;
            *sjw      = 1;
            *nosamp   = 1;
            *syncMode = 0;
            break;
            
        default:
            return canERR_PARAM;
    }
    
    return canOK;
}





#pragma mark - Leaf USB functions


IOReturn LeafWriteCommandToBulkPipe(Can4osxUsbDeviceHandleEntry *self, leafCmd cmd )
{
    IOReturn retVal = kIOReturnSuccess;
    IOUSBInterfaceInterface **interface = self->can4osxInterfaceInterface;
    
    if( self->endpoitBulkOutBusy == FALSE ) {
        
        self->endpoitBulkOutBusy = TRUE;
    
        retVal = (*interface)->WritePipe( interface, self->endpointNumberBulkOut, &cmd, cmd.head.cmdLen );
    
        if (retVal != kIOReturnSuccess)
        {
            CAN4OSX_DEBUG_PRINT("Unable to perform synchronous bulk write (%08x)\n", retVal);
            (void) (*interface)->USBInterfaceClose(interface);
            (void) (*interface)->Release(interface);
        }
        
        self->endpoitBulkOutBusy = FALSE;
    } else {
        //Endpoint busy
        LeafPrivateData *priv = (LeafPrivateData *)self->privateData;
        LeafWriteCommandBuffer(priv->cmdBufferRef, cmd);
    }
    
    return retVal;
}


#pragma mark - Leaf stuff
static UInt16 LeafFillBulkPipeBuffer(LeafCommandMsgBuf* bufferRef, UInt8 *pipe, UInt16 maxPipeSize)
{
    UInt16 fillState = 0;
    
    while ( fillState < maxPipeSize ) {
        leafCmd cmd;
        if ( LeafReadCommandBuffer(bufferRef, &cmd) ) {
            memcpy(pipe, &cmd, cmd.head.cmdLen);
            fillState += cmd.head.cmdLen;
            pipe += cmd.head.cmdLen;
            //Will another command fir in the pipe?
            if ( (fillState + sizeof(leafCmd)) >= maxPipeSize ) {
                *pipe = 0;
                break;
            }
            
        } else {
            *pipe = 0;
            break;
        }
    }
    
    return fillState;
}

static void LeafBulkWriteCompletion(void *refCon, IOReturn result, void *arg0)
{
    Can4osxUsbDeviceHandleEntry *self = (Can4osxUsbDeviceHandleEntry *)refCon;
    IOUSBInterfaceInterface **interface = self->can4osxInterfaceInterface;
    
    UInt32 numBytesWritten = (UInt32) arg0;
    
    (void)numBytesWritten;
    
    CAN4OSX_DEBUG_PRINT("Asynchronous bulk write complete\n");
    
    if (result != kIOReturnSuccess)
    {
        CAN4OSX_DEBUG_PRINT("error from asynchronous bulk write (%08x)\n", result);
        (void) (*interface)->USBInterfaceClose(interface);
        (void) (*interface)->Release(interface);
        return;
    }
    
    self->endpoitBulkOutBusy = FALSE;
    
    CAN4OSX_DEBUG_PRINT("Wrote %ld bytes to bulk endpoint\n", (long)numBytesWritten);
    
    LeafWriteToBulkPipe(self);
    
}



static IOReturn LeafWriteToBulkPipe(Can4osxUsbDeviceHandleEntry *self)
{
    IOReturn retval = kIOReturnSuccess;
    IOUSBInterfaceInterface **interface = self->can4osxInterfaceInterface;
    LeafPrivateData *priv = (LeafPrivateData *)self->privateData;
    
    if ( self->endpoitBulkOutBusy == FALSE ) {
        self->endpoitBulkOutBusy = TRUE;
        
        if (0 < LeafFillBulkPipeBuffer(priv->cmdBufferRef, self->endpointBufferBulkOutRef, self->endpointMaxSizeBulkOut )) {

            retval = (*interface)->WritePipeAsync(interface, self->endpointNumberBulkOut, self->endpointBufferBulkOutRef, self->endpointMaxSizeBulkOut, LeafBulkWriteCompletion, (void*)self);
        
            if (retval != kIOReturnSuccess) {
                CAN4OSX_DEBUG_PRINT("Unable to perform asynchronous bulk write (%08x)\n", retval);
                (void) (*interface)->USBInterfaceClose(interface);
                (void) (*interface)->Release(interface);
            }
        } else {
            self->endpoitBulkOutBusy = FALSE;
        }
    }
    
    return retval;
}


//Go bus on
canStatus LeafCanStartChip(CanHandle hdl)
{
    int retVal = 0;
    leafCmd cmd;
    Can4osxUsbDeviceHandleEntry *self = &can4osxUsbDeviceHandle[hdl];
    
    cmd.head.cmdNo            = CMD_START_CHIP_REQ;
    cmd.startChipReq.cmdLen   = sizeof(cmdStartChipReq);
    cmd.startChipReq.channel  = 0;
    cmd.startChipReq.transId  = 0;
    
    retVal = LeafWriteCommandToBulkPipe( self, cmd);
    
    return retVal;
}


//Set bit timing
canStatus LeafCanSetBusParams ( const CanHandle hnd, SInt32 freq, unsigned int tseg1,
                             unsigned int tseg2, unsigned int sjw,
                             unsigned int noSamp, unsigned int syncmode )
{
    leafCmd        cmd;
    UInt32         tmp, PScl;
    int            retVal;
    Can4osxUsbDeviceHandleEntry *self = &can4osxUsbDeviceHandle[hnd];
    
    CAN4OSX_DEBUG_PRINT("leaf: _set_busparam\n");
    
    if ( canOK != LeafCanTranslateBaud( &freq, &tseg1, &tseg2, &sjw, &noSamp, &syncmode)) {
        // TODO
        CAN4OSX_DEBUG_PRINT(" can4osx strange bitrate\n");
        return -1;
    }
    
    
    // Check bus parameters
    tmp = freq * (tseg1 + tseg2 + 1);
    if (tmp == 0) {
        CAN4OSX_DEBUG_PRINT("leaf: _set_busparams() tmp == 0!\n");
        return VCAN_STAT_BAD_PARAMETER;
    }
    
    PScl = 16000000UL / tmp;
    
    if (PScl <= 1 || PScl > 256) {
        CAN4OSX_DEBUG_PRINT("hwif_set_chip_param() prescaler wrong (%d)\n",PScl & 1 /* even */);
        return VCAN_STAT_BAD_PARAMETER;
    }
    
    cmd.setBusparamsReq.cmdNo   = CMD_SET_BUSPARAMS_REQ;
    cmd.setBusparamsReq.cmdLen  = sizeof(cmdSetBusparamsReq);
    cmd.setBusparamsReq.bitRate = freq;
    cmd.setBusparamsReq.sjw     = (UInt8)sjw;
    cmd.setBusparamsReq.tseg1   = (UInt8)tseg1;
    cmd.setBusparamsReq.tseg2   = (UInt8)tseg2;
    cmd.setBusparamsReq.channel = (UInt8)0;//vChan->channel;
    cmd.setBusparamsReq.noSamp  = 1; // qqq Can't be trusted: (BYTE) pi->chip_param.samp3
    
    CAN4OSX_DEBUG_PRINT("leaf_set_busparams: Chan(%d): Freq (%d) SJW (%d) TSEG1 (%d) TSEG2 (%d) Samp (%d)\n",
                        cmd.setBusparamsReq.channel,
                        cmd.setBusparamsReq.bitRate,
                        cmd.setBusparamsReq.sjw,
                        cmd.setBusparamsReq.tseg1,
                        cmd.setBusparamsReq.tseg2,
                        cmd.setBusparamsReq.noSamp);
    
    retVal = LeafWriteCommandToBulkPipe( self, cmd);
    
    return retVal;
}



void BulkReadCompletion(void *refCon, IOReturn result, void *arg0)
{
    Can4osxUsbDeviceHandleEntry *self = (Can4osxUsbDeviceHandleEntry *)refCon;
    IOUSBInterfaceInterface **interface = self->can4osxInterfaceInterface;
    UInt32 numBytesRead = (UInt32) arg0;
    
    CAN4OSX_DEBUG_PRINT("Asynchronous bulk read complete (%ld)\n", (long)numBytesRead);
    
    if (result != kIOReturnSuccess) {
        printf("Error from async bulk read (%08x)\n", result);
        (void) (*interface)->USBInterfaceClose(interface);
        (void) (*interface)->Release(interface);
        return;
    }
    
    if(numBytesRead > 0) {
        int count = 0;
        leafCmd *cmd;
        int loopCounter = 512;
        
        while ( count < numBytesRead ) {
            if (loopCounter-- == 0) break;
            
            cmd = (leafCmd *)&(self->endpointBufferBulkInRef[count]);//&gBufferReceive[count];
            if ( cmd->head.cmdLen == 0 ) {
                count += 512;
                count &= -512;
                continue;
            } else {
                count += cmd->head.cmdLen;
            }
            
            LeafDecodeCommand(self, cmd);
        }
    }
    
    LeafReadFromBulkInPipe(self);
}

void LeafReadFromBulkInPipe(Can4osxUsbDeviceHandleEntry *self)
{
    IOReturn ret = (*(self->can4osxInterfaceInterface))->ReadPipeAsync(self->can4osxInterfaceInterface, self->endpointNumberBulkIn, self->endpointBufferBulkInRef, self->endpointMaxSizeBulkIn, BulkReadCompletion, (void*)self);
    
    if (ret != kIOReturnSuccess) {
        CAN4OSX_DEBUG_PRINT("Unable to read async interface (%08x)\n", ret);
    }
}














