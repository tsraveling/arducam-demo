// ArduCAM Mini demo (C)2016 Lee
// web: http://www.ArduCAM.com
// This program is a demo of how to use most of the functions
// of the library with ArduCAM ESP8266 2MP camera.
// This demo was made for ArduCAM ESP8266 OV2640 2MP Camera.
// It can take photo and send to the Web.
// It can take photo continuously as video streaming and send to the Web.
// The demo sketch will do the following tasks:
// 1. Set the camera to JEPG output mode.
// 2. if server.on("/capture", HTTP_GET, serverCapture),it can take photo and send to the Web.
// 3.if server.on("/stream", HTTP_GET, serverStream),it can take photo continuously as video 
//streaming and send to the Web.

// This program requires the ArduCAM V4.0.0 (or later) library and ArduCAM ESP8266 2MP camera
// and use Arduino IDE 1.5.8 compiler or above

// This program has been refactored for clarity and heavily annotated by Tim Raveling.
// https://github.com/tsraveling

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <Wire.h>
#include <ArduCAM.h>
#include <SPI.h>
#include "memorysaver.h"

// This demo can only work with the ESP8266 board
#if !(defined ESP8266 )
#error Please select the ArduCAM ESP8266 UNO board in the Tools/Board
#endif

//This demo can only work on OV2640_MINI_2MP or ARDUCAM_SHIELD_V2 platform.
#if !(defined (OV2640_MINI_2MP)||(defined (ARDUCAM_SHIELD_V2) && defined (OV2640_CAM)))
#error Please select the hardware platform and camera module in the ../libraries/ArduCAM/memorysaver.h file
#endif


// Set the pin to 16 (on the ESP8266 board this is pretty much required)
const int CS = 16;

// Set whether this board creates it's own wifi network (AP, value 1 here) or functions as a station on an extant one (0)
int wifiType = 0; // 0: Station  1: AP

// AP mode configuration. You can set the ssid and password of the generated wifi network here. A password with zero length ("")
// will result in an open network.
const char *AP_ssid = "arducam_esp8266"; 
const char *AP_password = "";

// Station mode configuration. Put the ssid and WIFI password of your local network here in order to set it up on an extant
// wifi network. Note that the Serial Monitor will spit out the IP address of the ArduCAM once it starts up.
const char *ssid = "SSID";
const char *password = "PASSWORD";

// This board has a custom web server that we'll use to stream data
ESP8266WebServer server(80);

// Initiate the camera object
ArduCAM myCAM(OV2640, CS);

// This function starts the actual capture process
void start_capture(){
  myCAM.clear_fifo_flag();
  myCAM.start_capture();
}

// This function checks the size to make sure it's valid
bool checkSize(size_t len) {

  // Read image length from 0x42 - 0x44 in order to make sure it's a valid size.
  if (len >= 0x07ffff){
    Serial.println("Over size.");
    return false;
  }else if (len == 0 ){
    Serial.println("Size is 0.");
    return false;
  }

  return true;
}

// This function gets called at the start of a data transfer operation
void startTransfer() {
  
  // Pull the CS pin low to start the transfer
  myCAM.CS_LOW();

  // Send FIFO burst to 0x3C
  myCAM.set_fifo_burst();

  // Send 0xFF over the SPI if required
  #if !(defined (ARDUCAM_SHIELD_V2) && defined (OV2640_CAM))
    SPI.transfer(0xFF);
  #endif
}

// This function handles passing image data to the server
void camCapture(ArduCAM myCAM){

  // Get the wifi client
  WiFiClient client = server.client();

  // Get image length and make sure it's valid
  size_t len = myCAM.read_fifo_length();
  if (!checkSize(len)) return;

  // Start the transfer
  startTransfer();

  // Make sure we're connected to the wifi still
  if (!client.connected()) return;

  // Send a 200 response from our little mini-server
  String response = "HTTP/1.1 200 OK\r\n";
  response += "Content-Type: image/jpeg\r\n";
  response += "Content-Length: " + String(len) + "\r\n\r\n";
  server.sendContent(response);

  // Set up our buffers
  static const size_t bufferSize = 4096;
  static uint8_t buffer[bufferSize] = {0xFF};

  // Loop through the size of the data we've received
  while (len) {

      // Grab the next chunk of data
      size_t amountToCopy = (len < bufferSize) ? len : bufferSize;
      SPI.transferBytes(&buffer[0], &buffer[0], amountToCopy);

      // Make sure we're still connected to the wifi client
      if (!client.connected()) break;

      // Send the data into our response
      client.write(&buffer[0], amountToCopy);

      // Decrement the data left to copy and continue
      len -= amountToCopy;
  }

  // Pull the CS pin back high to signal completion of transfer
  myCAM.CS_HIGH();
}

void serverStream(){
  WiFiClient client = server.client();

  // Compose the initial 200 for up front connection
  String response = "HTTP/1.1 200 OK\r\n";
  response += "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
  server.sendContent(response);

  // Then, continuously send image data to the server.
  while (1) {

    // Send capture command
    start_capture();

    // Poll capture completion flag by reading 0x41 register until it equals 0x08
    while (!myCAM.get_bit(ARDUCHIP_TRIG, CAP_DONE_MASK));

    // Get the length and make sure it's valid
    size_t len = myCAM.read_fifo_length();
    if (!checkSize(len)) continue;

    // Start the transfer
    startTransfer();

    // Make sure the wifi client is connected
    if (!client.connected()) break;

    // Form the response headers
    response = "--frame\r\n";
    response += "Content-Type: image/jpeg\r\n\r\n";
    server.sendContent(response);

    // Set up the buffer
    static const size_t bufferSize = 4096;

    // Initialize the buffer with blanked (0xFF) values; this'll come in handy in a sec.
    static uint8_t buffer[bufferSize] = {0xFF};

    // Run through the length of the image (which we received earlier)
    while (len) {

      // We're either capturing a full chunk of data (bufferSize) or else whatever's left that's less than that (len)
      size_t amountToCopy = (len < bufferSize) ? len : bufferSize;

      // This function is defined in ArduCAM.cpp at line 3213. It's arguments are out, in, and size. It will go through the
      // chip-side storage up to a length of {size}. It will copy the camera's storage into data starting at the pointer {in}.
      // The end result of this is that the chunk of data (already encoded as image/jpeg) will now be in the buffer.
      SPI.transferBytes(&buffer[0], &buffer[0], amountToCopy);

      // Make sure we're still connected via wifi
      if (!client.connected()) break;

      // Copy the JPEG data we've received from the chip onto the open server datastream
      client.write(&buffer[0], amountToCopy);

      // Decrease our counter
      len -= amountToCopy;
    }

    // Pull the CS pin back high to signal that data tranasfer is completed
    myCAM.CS_HIGH();

    // Once again check for data connection.
    if (!client.connected()) break;
  }
}

// This function wraps the beefier camCapture function in order to get some performance data
// out of our chip.
void serverCapture(){
  
  start_capture();
  Serial.println("CAM Capturing");

  int total_time = 0;

  total_time = millis();
  while (!myCAM.get_bit(ARDUCHIP_TRIG, CAP_DONE_MASK));
  total_time = millis() - total_time;
  Serial.print("capture total_time used (in miliseconds):");
  Serial.println(total_time, DEC);
  
  total_time = 0;
  
  Serial.println("CAM Capture Done!");
  total_time = millis();
  camCapture(myCAM);
  total_time = millis() - total_time;
  Serial.print("send total_time used (in miliseconds):");
  Serial.println(total_time, DEC);
  Serial.println("CAM send Done!");
}

// This functions as a sort of catchall / default endpoint rather than the more
// standard dedicated 404 response. This endpoint will simply inform the client
// that the server is running and update the quality level if needed.
void handleNotFound() {
  
  String message = "Server is running!\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  server.send(200, "text/plain", message);

  // If we've received a `ql` argument, we can update the camera's quality level.
  if (server.hasArg("ql")){

    // Convert the argument to an int.
    int ql = server.arg("ql").toInt();

    // Send that to the camera object
    myCAM.OV2640_set_JPEG_size(ql);

    // Wait a second, then let the Monitor know what happened
    delay(1000);
    Serial.println("QL change to: " + server.arg("ql"));
  }
}


// Called when the board initializes
void setup() {
  
  uint8_t vid, pid;
  uint8_t temp;

  // Use Wire1 if we're using a SAM3X8E microcontroller, otherwise use a regular Wire
  #if defined(__SAM3X8E__)
    Wire1.begin();
  #else
    Wire.begin();
  #endif

  // Start Serial on 115200 baud (don't forget to set your Serial Monitor accordingly)
  Serial.begin(115200);
  Serial.println("ArduCAM Start!");

  // Set the CS pin as an output:
  pinMode(CS, OUTPUT);

  // Innitialize the SPI:
  SPI.begin();
  SPI.setFrequency(4000000); //4MHz

  // Make sure the ArduCAM SPI bus is OK
  myCAM.write_reg(ARDUCHIP_TEST1, 0x55);
  temp = myCAM.read_reg(ARDUCHIP_TEST1);
  if (temp != 0x55){
    Serial.println("SPI1 interface Error!");
    while(1);
  }

  // Make sure we're actually using a OV2640 camera module
  myCAM.wrSensorReg8_8(0xff, 0x01);
  myCAM.rdSensorReg8_8(OV2640_CHIPID_HIGH, &vid);
  myCAM.rdSensorReg8_8(OV2640_CHIPID_LOW, &pid);
  if ((vid != 0x26 ) && (( pid != 0x41 ) || ( pid != 0x42 )))
    Serial.println("Can't find OV2640 module!");
  else
    Serial.println("OV2640 detected.");

  //Change to JPEG capture mode and initialize the OV2640 module
  myCAM.set_format(JPEG);
  myCAM.InitCAM();
  myCAM.OV2640_set_JPEG_size(OV2640_320x240);
  myCAM.clear_fifo_flag();

  // Set up our server based on whether we decided to use the station (0) or AP (1) model
  if (wifiType == 0) {

    // Make sure we've entered wifi credentials
    if(!strcmp(ssid,"SSID")){
       Serial.println("Please set your SSID");
       while(1);
    }
    if(!strcmp(password,"PASSWORD")){
       Serial.println("Please set your PASSWORD");
       while(1);
    }
    
    // Let the Monitor know what's going on
    Serial.println();
    Serial.println();
    Serial.print("Connecting to ");
    Serial.println(ssid);

    // Connect to the WiFi network specified
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    // If we can't connect immediately, wait half a second and try again    
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }

    // Once we have connected, let the Monitor know and continue on
    Serial.println("WiFi connected");
    Serial.println("");

    // Kick the local IP of the Arducam board to the Monitor so we know how to connect to it
    Serial.println(WiFi.localIP());
    
  } else if (wifiType == 1) {

    // For AP mode, we're generating our own wifi network, so kick the credentials we've defined to
    // the Monitor for convenience' sake.
    Serial.println();
    Serial.println();
    Serial.print("Share AP: ");
    Serial.println(AP_ssid);
    Serial.print("The password is: ");
    Serial.println(AP_password);

    // Let the WiFi object know to start it's own network
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_ssid, AP_password);
    Serial.println("");
    Serial.println(WiFi.softAPIP());
  }
  
  // Start the server and define our endpoints (for image, stream, and 404 respectively)
  server.on("/capture", HTTP_GET, serverCapture);
  server.on("/stream", HTTP_GET, serverStream);
  server.onNotFound(handleNotFound);
  server.begin();

  // Let the Monitor know we're done
  Serial.println("Server started");
}

// Standard Arduino loop function
void loop() {

  // Let our little mini-server know to listen to it's endpoints, as everything on this particular project
  // is initiated through hitting those via a browser.
  server.handleClient();
}
