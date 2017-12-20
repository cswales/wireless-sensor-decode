''' GE Wireless sensor 
    Maintains list of wireless sensors that we're listening to.
    Keeps track of current state of sensors. Can send events when sensors open or close,
    provide sensor state, learn (or unlearn) sensors.
    XXX - what about history? That would be handy to have as well.
'''
import os
import getopt
import sys
import json
from threading import Thread
from aiohttp import web
import traceback
import time
import datetime

import logging

logging.basicConfig()
logger = logging.getLogger()

sensorDB = None
    
def writeSensorList():
    try:
        with open(dbFile, "w+") as f:
            json.dump(sensorList,f)
    except Exception as e:
        logger.warn("Cound not write file {}, exception {}".format(dbFile, e))
        raise e

def printUsage():
    sys.strerr.write("Usage:")
    sys.strerr.write("{} [-d dbFile] [-n] [-i input] [-o output] [-p httpPort]".format(sys.argv[0]))
    sys.strerr.write(" 'dbFile' is the name of the database file. Defaults to sensordb.json. Will")
    sys.strerr.write("          be created if it does not already exist")
    sys.strerr.write(" 'input' is where to find the stream of sensor events. Defaults to stdin, but")
    sys.strerr.write("          can also read from a file")
    sys.strerr.write(" 'output' is where to stream formatted event stream. Defaults to stdout, but")
    sys.strerr.write("          can be a POST http url or file.")
    sys.strerr.write(" 'httpPort' is the port where the webserver and REST commands reside")
    sys.strerr.write(" ")
    
def main():
    global sensorDB

    dbFile =  "./sensordb.json"
    
    try:
        opts, args = getopt.getopt(sys.argv[1:], "hvd:i:o:p:")
    except getopt.GetoptError as err:
        # print help information and exit:
        sys.strerr.write(str(err))  # will print something like "option -a not recognized"
        printUsage()
        sys.exit(2)
        
    httpPort = 9000
    input = sys.stdin
    output = sys.stdout
    verbose = False
    for o, a in opts:
        if o == "-v":
            verbose = True
        elif o =="-h":
            printUsage()
            sys.exit()
        elif o == "-o":
            output = a
        elif o == "-i":
            input = a
        elif o == "-p":
            try:
                port = int(a)
            except Exception:
                sys.strerr.write("Port must be a valid integer")
                sys.exit(2)
        elif o == "-d":
            dbFile = a
        else:
            assert False, "unhandled option"
    
    try:
        sensorDB = SensorDB(dbFile)
    except Exception as e:
        logger.error("Error setting up db, aborting", e)
        sys.exit(3)
     
    try:   
        readSensorEvents(input)
    except KeyboardInterrupt:
        pass
    except Exception as e:
        logger.error("Error reading from input stream", e)
            

    # spin up thread to read input, create history (okay, fine, I need a simple db)
    # do webserver at specified port
    # done
class RecordNotFoundError(Exception):
    def __init__(self):
        pass
        
class DuplicateRecordError(Exception):
    def __init__(self):
        pass

class SensorDB:
    def __init__(self, filename):
        self.validSensorTypes = ["D/W", "Motion"] # XXX this is a class variable, not instance
        self.filename = filename
        self.sensors = {}
        
        try:
            with open(self.filename, "r") as f:
                self.sensors = json.load(f)
        except FileNotFoundError as e:
            try:
                self.dumpSensors()  # create file with no sensors
            except Exception as err:
                logger.error("{}. Could not open or create {}".format(err, self.filename))
                sys.exit(3)
        except ValueError as e:
            logger.error("{} Corrupt db file {}".format(e, self.filename))
            raise Exception # what kind XXX
        
    def addSensor(self, sensorId, timestamp, sensorState=None, rawData=None, sensorType="D/W"):
        if (sensorId in self.sensors):
            raise DuplicateRecordError()
 
        
        newSensor = {}
        newSensor["id"]         = sensorId
        newSensor["timestamp"]  = timestamp
        newSensor["type"]       = sensorType
        newSensor["raw"]    = rawData

        if (sensorState != None):
            newSensor["tampered"]   = sensorState["tampered"]
            newSensor["triggered"]  = sensorState["triggered"]
            newSensor["online"]     = True
            newSensor["lowBattery"] = sensorState["lowBattery"]

        self.sensors[sensorId] = newSensor
        
    def getSensorList(self):
        pass
        
    def getSensor(self, sensorId):
        try:
            return self.sensors[sensorId] 
        except KeyError:
            raise RecordNotFoundError()
        
    def modifySensor(self, sensorId, 
                           triggered=None, 
                           tampered=None, 
                           sensorType=None,
                           lowBattery=None,
                           online=None,
                           rawData=None,
                           timestamp=None):
        try:
            sensor = self.sensors[sensorId]
        except KeyError:
            raise RecordNotFoundError()  # XXX I'm sure there's something better, but...
                       
        if triggered != None and type(triggered) == bool:
            sensor["triggered"] = triggered
            
        if tampered != None and type(tampered) == bool:
            sensor["tampered"] = tampered
            
        if lowBattery != None and type(lowBattery) == bool:
            sensor["lowBattery"] = lowBattery
            
        if online != None and type(online) == bool:
            sensor["online"] = online
            
        if sensorType != None and sensorType in self.validSensorTypes:
            sensor["type"] = sensorType
            
        if rawData != None:
            sensor["raw"] = rawData
            
        if timestamp == None:
            timestamp = time.time()
            
        sensor["timestamp"] = timestamp 
        
        # XXX could raise errors if the types are wrong.
        
    def deleteSensor(self, sensorId):
        self.sensors.pop(sensorId, None)
        
    def dumpSensors(self):
        try:
            with open(self.filename, "w+") as f:
                json.dump(self.sensors, f)
        except Exception as e:
            logger.error("Cound not write file {}, exception {}".format(self.filename, e))
            raise e    
            
#def decodeEvent(rawData):
#    ''' At the moment, I really don't know how to decode what I'm seeing. So just set raw'''
#    return {"raw", rawData}

# for initial debugging - name these things
sensorNames = {"6f7c4e":"Kitchen Motion", "6847cd":"Back Window", "224fb5":"Front Door", "411d55":"Bathroom", "2c9bfd":"Back Door"}


learnMode = False
learnModeStartTime = None  # XXX should be initialized
learnModeTimeout = 30 # 30 seconds in learn mode

def enterLearnMode():
    global learnMode
    global learnModeStartTime
    learnMode = True
    learnModeStartTime = time.time()

def exitLearnMode():
    global learnMode
    learnMode = False
    
def deleteSensor(id):
    try:
        sensorDB.deleteSensor(id)
    except RecordNotFoundError:
        log.warn("Could not delete sensor {}, not found in db".format(id))
        
def postAddEvent(sensorId, timestamp, rawData):
    date = datetime.datetime.fromtimestamp(timestamp).strftime('%m-%d %H:%M:%S')
    try :
        name =sensorNames[sensorId]
    except KeyError:
        name = "Unknown"
    print("Add    {}, data {}, {} [{}]".format(sensorId, rawData, date, name))
    
def postModifyEvent(sensorId, timestamp, rawData):
    date = datetime.datetime.fromtimestamp(timestamp).strftime('%m-%d %H:%M:%S')
    try :
        name =sensorNames[sensorId]
    except KeyError:
        name = "Unknown"
    print("Modify {}, data {}, {} [{}]".format(sensorId, rawData, date, name ))

# note that I have no timer for exiting learn mode here
def handleEvent(event):
    global learnMode
    sensorId  = event["id"]
    timestamp = event["ts"]
    rawData   = event["raw"]
    
#    sensorState = decodeEvent(rawData)

#    if (learnMode and time.time() > learnModeStartTime + learnModeTimeout):
#        learnMode = False
    
    try: 
        sensor = sensorDB.getSensor(sensorId)
        # modify if data has changed, or if more than 5 seconds has gone by since last chirp
        if (rawData != sensor["raw"] or timestamp > sensor["timestamp"] + 5):
            sensorDB.modifySensor(sensorId, timestamp, rawData=rawData)
            postModifyEvent(sensorId, timestamp, rawData)
            
    except RecordNotFoundError:
        if learnMode:
            sensorDB.addSensor(sensorId, timestamp, rawData=rawData) # XXX - deal with sensor already in DB
            postAddEvent(sensorId, timestamp, rawData)
#            learnMode = False
        else:
            logger.warn("received message from unknown sensor {}".format(sensorId))
    
    
def readSensorEvents(eventstream_identifier):
    enterLearnMode()
    f = None
    try:
        f = open(eventstream_identifier)
    except TypeError as e:
        f = eventstream_identifier
        
    try:
        for line in iter(f.readline, ''):
            #print(line)
            event = json.loads(line)
            handleEvent(event)
    except Exception as e:
        traceback.print_exc()
        print(type(e).__name__) # Check various types of exceptions

if __name__ == "__main__":
    main();
    
        


    