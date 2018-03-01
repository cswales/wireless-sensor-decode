/**
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
    definition (name: "GE Wireless Motion Detector", namespace: "medeasoftware", author: "CS Wales") {
        capability "Motion Sensor"
        capability "Sensor"
        capability "Battery"
        capability "Tamper Alert"
    }

    // simulator metadata
    simulator {
    }

    // UI tile definitions
    tiles(scale: 2) {
        multiAttributeTile(name:"motion", type: "generic", width: 6, height: 4){
            tileAttribute("device.motion", key: "PRIMARY_CONTROL") {
                attributeState("active",   label:'motion',    icon:"st.motion.motion.active",   backgroundColor:"#00A0DC")
                attributeState("inactive", label:'no motion', icon:"st.motion.motion.inactive", backgroundColor:"#CCCCCC")
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
                attributeState("tampered", label:'tamper', icon:"st.motion.motion.active",   backgroundColor:"#00A0DC")
                attributeState("ok",       label:'ok',     icon:"st.motion.motion.inactive", backgroundColor:"#CCCCCC")
            }
        }
        main "motion"
        details "motion"
    }
}

// Parse incoming device messages to generate events
def parse(String description) {
    def name = null
    def value = description
    def descriptionText = null
    def result = []
    
    def event = new groovy.json.JsonSlurper().parseText(description)
    if (event.motion != null){
        def isActive = (event.motion == true);
        result.add(createEvent(
            name: "motion",
            value: isActive ? "active" : "inactive",
            descriptionText: isActive ? "${device.displayName} detected motion" : "${device.displayName} motion has stopped"
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
