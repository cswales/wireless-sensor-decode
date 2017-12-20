# 
# GE Wireless sensor packet decoding
#

import logging
#import socket
#import struct
import time
import sys
import json

logging.basicConfig()
logger = logging.getLogger()

''' The wireless signal is encoded through a series of pulses. A long pulse indicates
the start of the message, and a shorter pulse terminates the message. The message itself -
as well as a 4-item checksum after the termination pulse - is encoded through a series of
very short pulses. The length of all of these pulses is the same, but the time between
the pulses comes in two flavors - long or short.

For our purposes here, we consider that information is carried by the time between the 
pulses, and that a interval is a 0 and a long interval is a 1. (It's unclear what the 
actual encoding is - using this encoding we can definitely see a stable 24-bit value
that probably represents the transmitter id, but attempts to match our decoded value
to the value printed on the sensor have so far been failures.)'''

# pulse lengths in microseconds
MIN_START_LEN = 700
MAX_START_LEN = 1200
MIN_STOP_LEN = 400
MAX_STOP_LEN = 500
MIN_DATA_PULSE_LEN = 65
MAX_DATA_PULSE_LEN = 150
MIN_LONG_LEN = 200
MAX_LONG_LEN = 340
MIN_SHORT_LEN = 60
MAX_SHORT_LEN = 190

MAX_MSG_BITS = 55
MAX_BITS = 59

# XXX - would like to do an enum here
states = ["pending signal", "bits", "pending end", "checksum"]
state = "pending signal"

packetBitCount = 0
lastSignalTime = 0

packet = []
listeners = []

class BitException(Exception):
    def __init__(self):
        pass

class SignalException(Exception):
    def __init__(self):
        pass

def reset():
    global lastSignalTime
    global packet
    global state
    
    lastSignalTime = 0
    packet = []
    state = "pending signal"

def emitBit(deltaTime):
    global packet
    
    if deltaTime >= MIN_LONG_LEN and deltaTime <= MAX_LONG_LEN:
        # emit 0
        packet.append(0)
    elif deltaTime >= MIN_SHORT_LEN and deltaTime <= MAX_SHORT_LEN:
        # emit 1
        packet.append(1)
    else:
        raise BitException()
        
def acceptSignal(startTime, length):
    global state
    global lastSignalTime
    global packet
        
    deltaTime = startTime - lastSignalTime
#    print("acceptSignal times: start {}, lastSignal {}".format(startTime, lastSignalTime))
    # what happens depends on the state
    
    try: 
        if state == "pending signal":
            if length <= MAX_START_LEN and length >= MIN_START_LEN:
                lastSignalTime = startTime + length
                state = "bits"
            else:
                raise SignalException()
        elif state == "bits": # XXX should I look for the sync for validity?
            # check - is this a valid signal?
            if length <= MAX_DATA_PULSE_LEN and length >= MIN_DATA_PULSE_LEN:
                emitBit(deltaTime)
                
                if len(packet) >= MAX_MSG_BITS-1:  # NB - last bit gets emitted when we have the end signal
                    state = "pending end"                
            else:
                raise SignalException()
        elif state == "pending end":
            # is this a valid stop signal?
            if length <= MAX_STOP_LEN and length >= MIN_STOP_LEN: # there's got to be a in range or something in python
                emitBit(deltaTime)
                state = "checksum"
            else:                                
                raise SignalException()

        elif state == "checksum":
            # check - is this a valid signal?
            if length <= MAX_DATA_PULSE_LEN and length >= MIN_DATA_PULSE_LEN:
                emitBit(deltaTime)

            if len(packet) >= MAX_BITS:
                processPacket()
                packet = []
                state = "pending signal"
    
        lastSignalTime = startTime + length 
#        print("have {} bits".format(len(packet)))

    except SignalException:
        logger.error("Unexpected signal {}, {}, in state {} ignoring".format(startTime, length, state))
        
    except BitException:
        logger.error("Invalid distance {} between packets".format(deltaTime))
        reset()
            
def registerListener(callback):
    global listeners
    
    #print("Registering listener")
    # XXX check that callback is a function, ideally with the correct number of parameters, etc
    listeners.append(callback)
    
    # really ought to have some way of identifying the callback owner, but whatever for this
            
def processPacket():
    ''' Translate array of 0's and 1's to bytes, and publish to any subscribers '''
    global packet
    
    msg = {}
    
    # pull out sensor id
    id = []
    #nibbles = []
    raw = []
    sensorIdStart = 12
    for i in range(0, 6):
        sensorIdByte = packet[sensorIdStart + 4*i : sensorIdStart + 4*i + 4]
        #print sensorIdByte
        id.append(sensorIdByte[0]*8 + sensorIdByte[1] * 4 + sensorIdByte[2] * 2 + sensorIdByte[3])        
    id = "".join('{:x}'.format(x) for x in id)

    # print remaining nibbles
    for i in range(9, len(packet)/4):
        nibble = packet[4*i : 4*(i+1)]
        raw.append(nibble)
        #print nibble
        #nibbles.append(nibble[0]*8 + nibble[1] * 4 + nibble[2] * 2 + nibble[3]);
        
        
    # pull out open/close state
#    open1 = packet[41]
#    open2 = packet[42]
    
    sys.stdout.flush()
    

#    if open1 != open2:
#        logger.error("Internal confusion about packet state, ignoring packet")
#        return
        
#    triggered = (open1 == 1)
    
    msg["id"] = id
#    msg["triggered"] = triggered
    msg["ts"] = time.time()
    msg["raw"] = raw
    
    print json.dumps(msg)
    
    for i in range(0,len(listeners)):
        listeners[i](msg)
        
        
DEFAULT_PORT = 5644  
# testing
go_right_full = [[0, 1000],  # start
            [1100, 100], [1400, 100], [1600, 100], [1800, 100], [2000, 100], [2200, 100],
            [2400, 100], [2600, 100], [2800, 100], [3000, 100], [3200, 100], [3400, 100], # 12 synch bits
            [3800, 100], [4200, 100], [4400, 100], [4800, 100], # 0010
            [5000, 100], [5200, 100], [5600, 100], [6000, 100], # 1100
            [6200, 100], [6600, 100], [7000, 100], [7200, 100], # 1001
            [7400, 100], [7800, 100], [8000, 100], [8200, 100], # 1011
            [8400, 100], [8600, 100], [8800, 100], [9000, 100], # 1111
            [9200, 100], [9400, 100], [9800, 100], [10000, 100], # 1101
            [10400, 100], [10800, 100], [11200, 100], [11600, 100], # 0000
            [11800, 100], [12200, 100], [12600, 100], [12800, 100], # 1001
            [13200, 100], [13400, 100], [13600, 100], [13800, 100], # 0111
            [14200, 100], [14400, 100], [14800, 100], [15000, 100], # 1010
            [15200, 100], [15600, 100], [15800, 100]]

go_right = [[0, 1000],
            [1100, 100], [1300, 100], [1500, 100], [1700, 100], [1900, 100], [2100, 100],
            [2300, 100], [2500, 100], [2700, 100], [2900, 100], [3100, 100], [3300, 100], # 12 synch bits
            [3700, 100], [4100, 100], [4300, 100], [4700, 100], # 0010
            [4900, 100], [5100, 100], [5500, 100], [5900, 100], # 1100
            [6100, 100], [6500, 100], [6900, 100], [7100, 100], # 1001
            [7300, 100], [7700, 100], [7900, 100], [8100, 100], # 1011
            [8300, 100], [8500, 100], [8700, 100], [8900, 100], # 1111
            [9100, 100], [9300, 100], [9700, 100], [9900, 100], # 1101
            [10300, 100], [10700, 100], [11100, 100], [11500, 100], # 0000
            [11700, 100], [12100, 100], [12500, 100], [12700, 100], # 1001
            [13100, 100], [13300, 100], [13500, 100], [13700, 100], # 0111
            [14100, 100], [14300, 100], [14700, 100], [14900, 100], # 0101
            [15100, 100], [15500, 100], [15700, 500], # 101
            [16300, 100], [16500, 100], [16900, 100], [17100, 100]] # 0101 (checksum)
            
exp = [[1640249, 1000],
[1641374, 63],
[1641624, 63],
[1641874, 63],
[1642124, 63],
[1642374, 63],
[1642624, 63],
[1642812, 125],
[1643062, 125],
[1643312, 125],
[1643562, 125],
[1643812, 62],
[1644062, 62],
[1644437, 62],
[1644687, 62],
[1644937, 62],
[1645249, 125],
[1645499, 125],
[1645874, 125],
[1646249, 125],
[1646624, 63],
[1646999, 63],
[1647249, 63],
[1647624, 63],
[1647937, 125],
[1648312, 125],
[1648562, 125],
[1648812, 125],
[1649062, 62],
[1649312, 62],
[1649562, 62],
[1649937, 62],
[1650312, 62],
[1650499, 125],
[1650749, 125],
[1651124, 125],
[1651374, 125],
[1651749, 63],
[1651999, 63],
[1652249, 63],
[1652624, 63],
[1652874, 63],
[1653124, 63],
[1653312, 125],
[1653562, 125],
[1653937, 125],
[1654187, 62],
[1654437, 62],
[1654687, 62],
[1655062, 62],
[1655312, 62],
[1655624, 125],
[1655874, 125],
[1656249, 125],
[1656624, 125],
[1656874, 438],
[1657624, 63],
[1657999, 63],
[1658249, 63],
[1658437, 125]]


def go_right_1():
    reset()
    msg = {}   
    
    def callback(data):
        print("Callback called")
        msg["foo"] = data  # nb - I can't reset the outside pointer to msg, but I can reset what's inside
        print("msg is ", msg)
         
    registerListener(callback)
    for signal in go_right:
        acceptSignal(signal[0], signal[1])
    
    # this is all single threaded, so I should have what I need when I get to here
    print msg
    

def realMain():
     
    def callback(data):
        pass
        #print data
        #msg["foo"] = data  # nb - I can't reset the outside pointer to msg, but I can reset what's inside
        #print("msg is ", msg)
         
    registerListener(callback)

    while 1:
        # read from sys.stdin
        # process
        # repeat
        try: 
            for line in iter(sys.stdin.readline, ''):
                signal = json.loads(line)
                #print signal
                acceptSignal(signal[0], signal[1])
        except KeyboardInterrupt:
            break

  

    
if __name__ == '__main__':
    #go_right_1()
    realMain()
    
                     

# check results
# test that I did get a callback
# test that the id is 0x4C9BFD
# test that triggered is False
reset()
