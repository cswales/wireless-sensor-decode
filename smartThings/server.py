import http.server
import socketserver
import http.client
import json
import logging
from cgi import parse_header, parse_multipart
from urllib.parse import parse_qs

PORT = 3001

subscriberList = []
sensorList = []

logging.basicConfig()
log = logging.getLogger()
log.setLevel(logging.DEBUG)

class SensorHandler(http.server.BaseHTTPRequestHandler):

    global subscriberList
    global sensorList

    def do_GET(self):
        log.debug("GET with path %s" % self.path)
        if self.path == "/devices":
            self.send_response(200)
            self.send_header("Content-type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"sensors":sensorList}).encode('utf-8'))
        else:
            self.send_response(404)
    
    def do_POST(self):
        log.debug("POST with path %s" % self.path)
        ctype, pdict = parse_header(self.headers['content-type'])
        log.debug("ctype is %s" % ctype)
        log.debug("content-length is %s" % self.headers['content-length'])
        if ctype == 'multipart/form-data':
            postvars = parse_multipart(self.rfile, pdict)
        elif ctype == 'application/x-www-form-urlencoded':
            length = int(self.headers['content-length'])
            postvars = parse_qs(
                    self.rfile.read(length).decode('utf-8'),
                    keep_blank_values=1)
        elif ctype == 'application/json':
            length = int(self.headers['content-length'])
            bodyStr = self.rfile.read(length).decode('utf-8')
            log.debug("body is %s" % bodyStr)
            postvars = json.loads(bodyStr)
        else:
            log.debug("No post vars")
            postvars = {}
        
        if self.path == "/subscribe":
            # Add subscriber to list if it isn't already there
            try :
                listenAddr = postvars["ipaddr"]
                listenPort = postvars["port"]
                if isinstance(listenAddr, list):
                    listenAddr = listenAddr[0]
                if isinstance(listenPort, list):
                    listenPort = listenPort[0]
            except KeyError:
                log.warn("Invalid addr/port for subscriber")
                log.debug("postvars are", postvars)
                self.send_response(400)
                return
            try:
                listenPath = postvars["path"]
                if isinstance(listenPath, list):
                    listenPath = listenPath[0]
            except KeyError:
                listenPath = None

            try:
                listenPort = int(listenPort)
            except ValueError:
                log.warn("Invalid port %s for subscriber" % listenPort)
                self.send_response(400)

            foundSubscriber = False
            log.debug("attempting to find subscriber")
            for subscriber in subscriberList:
                if (subscriber["addr"] == listenAddr and  
                   (subscriber["port"] == listenPort and subscriber["path"] == listenPath)):
                    foundSubscriber = True
                    break
            if not foundSubscriber:
                log.debug("Adding subscriber, addr:%s, port:%s, path%s" % (listenAddr, listenPort, listenPath))
                subscriberList.append({'addr':listenAddr, 'port':listenPort, 'path':listenPath})
            self.send_response(200)

        elif self.path == "/event":
            # update local sensor state with event data, send event out to subscribers
            log.debug(postvars)
            log.debug(postvars["sensorId"][0])
            try:
                sensorId   = postvars["sensorId"][0]
                eventType  = postvars["eventType"][0]
                eventState = postvars["state"][0]
            except KeyError:
                log.debug("Missing id, type, or state in event POST")
                self.send_response(400)
                return

            # convert eventState into appropriate type
            if eventState in ['True', 'true', 'TRUE']:
                eventState = True
            elif eventState in ['False', 'false', 'FALSE']:
                eventState = False
            else:
                try:
                    eventState = int(eventState)
                except ValueError:
                    try:
                        eventState = float(eventState)
                    except ValueError:
                        pass 

            # change internal representation of sensor state
            for sensor in sensorList:
                if sensor["id"] == sensorId:
                    try:
                        sensor[eventType] = eventState
                    except KeyError:
                        pass # for the moment, allow the event to continue to the subscribers. This is debug code

            # send event to subscribers
            for subscriber in subscriberList:
                event = {}
                event["id"] = sensorId
                event[eventType] = eventState
                self.sendTCPNotification(subscriber, event)
            self.send_response(200)
        else:
            self.send_response(404)


    def sendTCPNotification(self, subscriber, event):
        addr = subscriber["addr"]
        port = subscriber["port"]
        eventData = json.dumps(event) 
        
        log.debug("Sending TCP Event notification to subscriber addr %s, port %d event %s" %(addr, port, eventData))
        headers = { 'device':"RPI Wireless Sensors",
                'CONTENT-TYPE': "application/json",
                'CONTENT-LENGTH': len(eventData.encode('utf-8'))}
        connection = http.client.HTTPConnection(addr, port) 
        connection.request("NOTIFY", "/notify", eventData, headers)
        response = connection.getresponse()
        log.debug("TCP Notification returns %d" % response.status)

def main():
    global sensorList
    #with socketserver.TCPServer(("", PORT), SensorHandler) as httpd:
    sensorList =[{"id":"ABCD", "name":"Door Sensor",   "type":"dw",     "triggered":True, "tampered":False, "low_battery":True, "online":True, "snr":14.4},
                 {"id":"DEFG", "name":"Motion Sensor", "type":"motion", "triggered":True, "tampered":False, "low_battery":True, "online":True, "snr":18.4}]
 
    with http.server.HTTPServer(("", PORT), SensorHandler) as httpd:
        print("serving at port", PORT)
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass

if __name__ == '__main__':
    main()
