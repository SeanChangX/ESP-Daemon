#include "web_server.h"
#include "config.h"

#include "ros_node.h"

#include <SPIFFS.h>
#include <Arduino_JSON.h>
#include <ESPAsyncWebServer.h>
#include <AsyncElegantOTA.h>

AsyncWebServer server(80);
AsyncEventSource events("/events");

// For accessing the ROS landing gear message
extern std_msgs__msg__Bool landing_gear_msg;

void initSPIFFS() {
  if (!SPIFFS.begin()) {
    Serial.println("SPIFFS Mount Failed");
  }
}

void initWebServer() {
  // [ 2025-04-04 ]
  // |   Removed the initSPIFFS() call here as it is already invoked in main.cpp. 
  // |   This ensures SPIFFS is initialized before starting the web server and WiFi manager.
  // initSPIFFS();
  
  // Serve the main index page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(SPIFFS, "/index.html", "text/html");
  });

  // Serve static files from SPIFFS
  server.serveStatic("/", SPIFFS, "/");

  // Endpoint for sensor readings
  server.on("/readings", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "application/json", "{}");
  });

  // Landing gear endpoint - handles both form data and JSON requests
  server.on("/landing_gear", HTTP_POST, 
    [](AsyncWebServerRequest* request) {
      String response = "{\"success\":false,\"message\":\"Invalid request\"}";
      
      // Process form data submission
      if (request->hasParam("landing_gear", true)) {
        String landingGearValue = request->getParam("landing_gear", true)->value();
        
        // Set landing gear state based on parameter value
        if (landingGearValue == "true" || landingGearValue == "1") {
          landing_gear_msg.data = true;
          landing_gear_callback(&landing_gear_msg);
          response = "{\"success\":true,\"message\":\"Landing gear RETRACTED\"}";
          // Serial.println("Landing gear RETRACTED via web interface");
        } else if (landingGearValue == "false" || landingGearValue == "0") {
          landing_gear_msg.data = false;
          landing_gear_callback(&landing_gear_msg);
          response = "{\"success\":true,\"message\":\"Landing gear EXTENDED\"}";
          // Serial.println("Landing gear EXTENDED via web interface");
        }
      }
      
      // Process JSON submission (stored by body handler)
      if (request->_tempObject != nullptr) {
        String jsonStr = (const char*)request->_tempObject;
        JSONVar jsonObj = JSON.parse(jsonStr);
        
        if (JSON.typeof(jsonObj) == "object" && jsonObj.hasOwnProperty("landing_gear")) {
          bool landingGearState = (bool)jsonObj["landing_gear"];
          landing_gear_msg.data = landingGearState;
          
          // Call landing gear callback to ensure immediate action
          landing_gear_callback(&landing_gear_msg);
          
          String status = landingGearState ? "RETRACTED" : "EXTENDED";
          response = "{\"success\":true,\"message\":\"Landing gear " + status + "\"}";
          
          // Serial.print("Landing gear ");
          // Serial.print(status);
          // Serial.println(" via web interface (JSON)");
          
          // Broadcast change as SSE event
          JSONVar statusObj;
          statusObj["landing_gear"] = landingGearState;
          statusObj["status"] = status;
          events.send(JSON.stringify(statusObj), "landing_gear_update", millis());
        }
        
        // Free allocated memory
        free(request->_tempObject);
        request->_tempObject = nullptr;
      }
      
      request->send(200, "application/json", response);
    },
    nullptr, // No upload handler needed
    [](AsyncWebServerRequest* request, uint8_t *data, size_t len, size_t index, size_t total) {
      // Store incoming JSON data for processing in the main handler
      if (len && index == 0) {
        // First chunk - allocate memory
        request->_tempObject = malloc(total + 1);
        if (request->_tempObject) {
          memcpy(request->_tempObject, data, len);
          ((char*)request->_tempObject)[len] = 0; // Null terminator
        }
      } else if (len && request->_tempObject) {
        // Subsequent chunks - append data
        memcpy(((char*)request->_tempObject) + index, data, len);
        // Add null terminator if this is the last chunk
        if (index + len >= total) {
          ((char*)request->_tempObject)[total] = 0;
        }
      }
    }
  );

  // Configure SSE events
  events.onConnect([](AsyncEventSourceClient* client) {
    // if (client->lastId()) {
    //   Serial.printf("Client reconnected! Last ID: %u\n", client->lastId());
    // }
    client->send("connected", NULL, millis(), 10000);
  });

  // Add event handler and start the server
  server.addHandler(&events);
  AsyncElegantOTA.begin(&server);
  server.begin();
}
