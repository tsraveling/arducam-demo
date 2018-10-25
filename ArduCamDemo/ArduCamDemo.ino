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
const char *ssid = "";
const char *password = "";

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

  return true
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

  // Read image length from 0x42 - 0x44 and make sure it's a valid size
  size_t len = myCAM.read_fifo_length();
  if (!checkSize(size_t)) return;

  // Start the transfer
  startTransfer();
  
  if (!client.connected()) return;
  String response = "HTTP/1.1 200 OK\r\n";
  response += "Content-Type: image/jpeg\r\n";
  response += "Content-Length: " + String(len) + "\r\n\r\n";
  server.sendContent(response);
  
  static const size_t bufferSize = 4096;
  static uint8_t buffer[bufferSize] = {0xFF};
  
  while (len) {
      size_t will_copy = (len < bufferSize) ? len : bufferSize;
      SPI.transferBytes(&buffer[0], &buffer[0], will_copy);
      if (!client.connected()) break;
      client.write(&buffer[0], will_copy);
      len -= will_copy;
  }
  
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

void handleNotFound(){
  String message = "Server is running!\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  server.send(200, "text/plain", message);
  
  if (server.hasArg("ql")){
    int ql = server.arg("ql").toInt();
    myCAM.OV2640_set_JPEG_size(ql);delay(1000);
    Serial.println("QL change to: " + server.arg("ql"));
  }
}

void setup() {
  uint8_t vid, pid;
  uint8_t temp;
#if defined(__SAM3X8E__)
  Wire1.begin();
#else
  Wire.begin();
#endif
  Serial.begin(115200);
  Serial.println("ArduCAM Start!");

  // set the CS as an output:
  pinMode(CS, OUTPUT);

  // initialize SPI:
  SPI.begin();
  SPI.setFrequency(4000000); //4MHz

  //Check if the ArduCAM SPI bus is OK
  myCAM.write_reg(ARDUCHIP_TEST1, 0x55);
  temp = myCAM.read_reg(ARDUCHIP_TEST1);
  if (temp != 0x55){
    Serial.println("SPI1 interface Error!");
    while(1);
  }

  //Check if the camera module type is OV2640
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

  if (wifiType == 0){
    if(!strcmp(ssid,"SSID")){
       Serial.println("Please set your SSID");
       while(1);
    }
    if(!strcmp(password,"PASSWORD")){
       Serial.println("Please set your PASSWORD");
       while(1);
    }
    // Connect to WiFi network
    Serial.println();
    Serial.println();
    Serial.print("Connecting to ");
    Serial.println(ssid);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    Serial.println("WiFi connected");
    Serial.println("");
    Serial.println(WiFi.localIP());
  }else if (wifiType == 1){
    Serial.println();
    Serial.println();
    Serial.print("Share AP: ");
    Serial.println(AP_ssid);
    Serial.print("The password is: ");
    Serial.println(AP_password);
    
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_ssid, AP_password);
    Serial.println("");
    Serial.println(WiFi.softAPIP());
  }
  
  // Start the server
  server.on("/capture", HTTP_GET, serverCapture);
  server.on("/stream", HTTP_GET, serverStream);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Server started");
}

void loop() {
  server.handleClient();
}
