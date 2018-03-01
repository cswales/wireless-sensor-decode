/**
 *  GE Wireless Door/Window Sensor
 *
 *  Copyright 2017 CS Wales
 *
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
 *  in compliance with the License. You may obtain a copy of the License at:
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software distributed under the License is distributed
 *  on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the License
 *  for the specific language governing permissions and limitations under the License.
 *
 */
metadata {
    definition (name: "GE Wireless Door/Window Sensor", namespace: "medeasoftware", author: "CS Wales") {
        capability "Battery"
        capability "Contact Sensor"
        capability "Sensor"
        capability "Tamper Alert"
    }


    simulator {
    // Okay - so this simulator stuff completely blows because it appears there's no way to pass a JSON string
    // through the simulator. Good work guys.
        status "open":   "{\"type\":\"contact\", \"subtype\":\"open\"}"
        status "closed": '"type":"contact", "subtype":"closed"'
        status 'tamper': '{"type":"tamper", "subtype":"tampered"}'
        status "clear":  "type:'tamper', subtype:'clear'"
        status "batteryLow": "type:'lowbattery', subtype:'low battery'"
        status "batteryOk":  "type:'lowbattery'"
        status "online": "type:'supervisory', subtype:'online'"
        status "offline": "type:'supervisory', subtype:'offline'"
        status 'foo': ' '
    }

    tiles(scale: 2) {
        multiAttributeTile(name:"DW Sensor", type: "generic", width: 6, height: 4){
            tileAttribute("device.contact", key: "PRIMARY_CONTROL") {
                attributeState("closed",   label:'closed',    icon:"st.motion.motion.active",   backgroundColor:"#00A0DC")
                attributeState("open",     label:'open', icon:"st.motion.motion.inactive", backgroundColor:"#CCCCCC")
            }
            tileAttribute("device.supervisory", key: "SECONDARY_CONTROL") {
                attributeState("online",   label:'on line',    icon:"st.motion.motion.active",   backgroundColor:"#00A0DC")
                attributeState("offline",  label:'off line',   icon:"st.motion.motion.inactive", backgroundColor:"#CCCCCC")
            }
            tileAttribute("device.lowbattery", key: "SECONDARY_CONTROL") {
                attributeState("batteryLow", label:'low battery', icon:"st.motion.motion.active",   backgroundColor:"#00A0DC")
                attributeState("batteryOk",  label:'battery ok',  icon:"st.motion.motion.inactive", backgroundColor:"#CCCCCC")
            }
            tileAttribute("device.tamper", key: "SECONDARY_CONTROL") {
                attributeState("detected", label:'tamper', icon:"st.motion.motion.active",   backgroundColor:"#00A0DC")
                attributeState("clear",       label:'ok',     icon:"st.motion.motion.inactive", backgroundColor:"#CCCCCC")
            }
        }
    }  // XXX what are detail tiles?
       // XXX what is the scale attribute?
       // XXX - I need better icons!
}

// parse events into attributes
def parse(String description) {
    log.debug "Parse called"
    log.debug("Trimmed string is ${description?.trim()}")
    if( !description?.trim()) {
        description = '{"id":"ABCD", "triggered":false, "tampered":false, "low_battery":false, "online":true, "snr":15.7}'
    }
    log.debug "Parsing '${description}'"
    log.debug "displayname is ${device.displayName}"

    def name = null
    def value = description
    def descriptionText = null
    def result = []
    
    def event = new groovy.json.JsonSlurper().parseText(description)  
    if (event.triggered != null) {
        def isOpen = event.triggered == true;
        result.add(createEvent(
            name : "contact",
            value : isOpen ? "open" : "closed",
            descriptionText : isOpen ? "${device.displayName} opened" : "${device.displayName} closed"
        ))
    } 
    if (event.online != null) {
        def isOnline = event.online == true;
        result.add(createEvent(
            name : "supervisory",
            value : isOnline ? "online" : "offline",
            descriptionText : isOnline ? "${device.displayName} has come online" : "${device.displayName} has gone offline"
        ))
    } 
    if (event.low_battery != null) {
        def isLowBattery = event.low_battery != true;
        result.add(createEvent(
            name : "lowbattery",
            value : isLowBattery ? "batteryLow" : "batteryOk",
            descriptionText : isLowBattery ? "${device.displayName} : low battery" : "${device.displayName} : battery ok"
        ))
    } 
    if (event.tampered != null) {
        def isTampered = event.tampered == true;
        result.add(createEvent(
            name : "tamper",
            value : isTampered ? "detected" : "clear",
            descriptionText : isTampered ? "${device.displayName} : tampered" : "${device.displayName} : tamper restored"
        ))  
    }

    log.debug "Parse returned ${result?.descriptionText}"
    return result
}

