#include <Arduino.h>
#include <Adafruit_DotStar.h>
#include <SPI.h>
#include <IRLib2.h>
#include <IRLibRecv.h>
#include <IRLibDecodeBase.h> 
#include <IRLib_P01_NEC.h>

/* 
 *  2020-12-16 trinketm0 poi 50px 
Readme: 
MCU: TrinketM0
NUMLEDS=36
LED Strip IC: SK9822
IR Sensor and remote 
Patterns: loops through multiple POV adafruit examples

Changelog:
IR: seems to print 10 commands to serial but doesn't affect strip
6/26: serial monitor shows button pressed and assigned function but doesn't affect LEDs. 
There is secondary logic to handle buttons but doesn't seem to be working.
6/28: LEDs are now responding to button presses. speed is not working. The Trinket M0 CPU is too slow? 
enabled SPI native, speed is now much faster. 
*/

typedef uint16_t line_t;

// CONFIGURABLE STUFF ------------------------------------------------------

#include "graphics.h" // Graphics data is contained in this header file.

// Trinket M0
#define LED_PIN_DATA     4
#define LED_PIN_CLOCK    3

// Huzzah32 
//#define LED_PIN_DATA     18
//#define LED_PIN_CLOCK    5


// Empty and full thresholds (millivolts) used for battery level display:
#define BATT_MIN_MV 3350 // Some headroom over battery cutoff near 2.9V
#define BATT_MAX_MV 4000 // And little below fresh-charged battery near 4.1V

boolean autoCycle = false; // Set to true to cycle images by default
#define CYCLE_TIME 5     // Time, in seconds, between auto-cycle images

// Int1 on Trinket M0 is hidden in the LED.
// eINT2 is on pin 3
//#define IR_PIN     3      // MUST be INT1 pin!
//IRrecv myReceiver(1); //create receiver and pass pin number
IRrecv myReceiver(1); //create receiver and pass pin number
IRdecode myDecoder;   //create decoder

// Original Remote 
//#define UpArrow     0xFDA05F
//#define DownArrow   0xFDB04F
//#define LeftArrow   0xFD10EF
//#define RightArrow  0xFD50AF
//#define EnterSave   0xFD906F
//#define VolumeUp    0xFD40BF
//#define VolumeDown  0xFD00FF
//#define Setup       0xFD20DF
//#define Restart     0xFD708F
//#define PlayPause   0xFD807F
//#define StopMode    0xFD609F
//#define Back        0xFD708F
//#define None        0xFFFFFFFF

// New Remote 
#define UpArrow     0xFFE21D // increase speed
#define DownArrow   0xFFA25D // decrease speed
#define LeftArrow   0xFF22DD //previous pattern
#define RightArrow  0xFF02FD // next pattern
#define VolumeUp    0xFFA857 // increase brightness
#define VolumeDown  0xFFE01F // decrease brightness
#define PlayPause   0xFFC23D // Toggle Autoplay
#define StopMode    0xFF6897 // Off

// not used in new remote
#define Setup       0xFD20DF // Battery
#define Back        0xFD708F // Restart
#define None        0xFFFFFFFF

// -------------------------------------------------------------------------
#define NUM_LEDS_TOTAL 177
#define NUM_LEDS 37

// bit banging
//Adafruit_DotStar strip = Adafruit_DotStar(NUM_LEDS, LED_DATA_PIN, LED_CLOCK_PIN, DOTSTAR_BRG);
// SPI Native
Adafruit_DotStar strip = Adafruit_DotStar(NUM_LEDS, DOTSTAR_BGR); 


void     imageInit(void),
         IRinterrupt(void);

void setup() {
  delay( 2000 );
  Serial.begin(9600);
  Serial.println(F("Serial Started."));
  strip.begin(); // Allocate DotStar buffer, init SPI
  strip.setBrightness(85); // 1/3 brightness
  strip.clear(); // Make sure strip is clear
  strip.show();  // before measuring battery

  imageInit();   // Initialize pointers for default image

  //attachInterrupt(1, IRinterrupt, CHANGE); // IR remote interrupt
  //delay(2000); while (!Serial); //delay for Leonardo
  myReceiver.enableIRIn(); // Start the receiver
  Serial.println(F("Ready to receive IR signals"));
}


// GLOBAL STATE STUFF ------------------------------------------------------

uint32_t lastImageTime = 0L, // Time of last image change
         lastLineTime  = 0L;
// shortcut single pattern
uint8_t  imageNumber   = 1,  // Current image being displayed
         imageType,          // Image type: PALETTE[1,4,8] or TRUECOLOR
        *imagePalette,       // -> palette data in PROGMEM
        *imagePixels,        // -> pixel data in PROGMEM
         palette[16][3];     // RAM-based color table for 1- or 4-bit images
line_t   imageLines,         // Number of lines in active image
         imageLine;          // Current line number in image
volatile uint16_t irCode = None; // Last valid IR code received

const uint8_t PROGMEM brightness[] = { 15, 31, 63, 127, 255 };
uint8_t bLevel = sizeof(brightness) - 1;

// Microseconds per line for various speed settings
const uint16_t PROGMEM lineTable[] = { // 375 * 2^(n/3)
  1000000L /  375, // 375 lines/sec = slowest
  1000000L /  472,
  1000000L /  595,
  1000000L /  750, // 750 lines/sec = mid
  1000000L /  945,
  1000000L / 1191,
  1000000L / 1500  // 1500 lines/sec = fastest
};
uint8_t  lineIntervalIndex = 3;
uint16_t lineInterval      = 1000000L / 750;

void imageInit() { // Initialize global image state for current imageNumber
  imageType    = pgm_read_byte(&images[imageNumber].type);
  imageLines   = pgm_read_word(&images[imageNumber].lines);
  imageLine    = 0;
  imagePalette = (uint8_t *)pgm_read_word(&images[imageNumber].palette);
  imagePixels  = (uint8_t *)pgm_read_word(&images[imageNumber].pixels);
  // 1- and 4-bit images have their color palette loaded into RAM both for
  // faster access and to allow dynamic color changing.  Not done w/8-bit
  // because that would require inordinate RAM (328P could handle it, but
  // I'd rather keep the RAM free for other features in the future).
  if(imageType == PALETTE1)      memcpy_P(palette, imagePalette,  2 * 3);
  else if(imageType == PALETTE4) memcpy_P(palette, imagePalette, 16 * 3);
  lastImageTime = millis(); // Save time of image init for next auto-cycle
}

void nextImage(void) {
  if(++imageNumber >= NUM_IMAGES) imageNumber = 0;
  imageInit();
}

void prevImage(void) {
  imageNumber = imageNumber ? imageNumber - 1 : NUM_IMAGES - 1;
  imageInit();
}

// MAIN LOOP ---------------------------------------------------------------

void loop() {
  uint32_t t = millis(); // Current time, microseconds
  IRinterrupt();
  
  if(autoCycle) {
    if((t - lastImageTime) >= (CYCLE_TIME * 1000L)) nextImage();
  }
  
  switch(imageType) {

    case PALETTE1: { // 1-bit (2 color) palette-based image
      uint8_t  pixelNum = 0, byteNum, bitNum, pixels, idx,
              *ptr = (uint8_t *)&imagePixels[imageLine * NUM_LEDS / 8];
      for(byteNum = NUM_LEDS/8; byteNum--; ) { // Always padded to next byte
        pixels = pgm_read_byte(ptr++);  // 8 pixels of data (pixel 0 = LSB)
        for(bitNum = 8; bitNum--; pixels >>= 1) {
          idx = pixels & 1; // Color table index for pixel (0 or 1)
          strip.setPixelColor(pixelNum++, palette[idx][0], palette[idx][1], palette[idx][2]);
          strip.setPixelColor((NUM_LEDS_TOTAL - pixelNum), palette[idx][0], palette[idx][1], palette[idx][2]);
        }
      }
      break;
    }

    case PALETTE4: { // 4-bit (16 color) palette-based image
      uint8_t  pixelNum, p1, p2,
              *ptr = (uint8_t *)&imagePixels[imageLine * NUM_LEDS / 2];
      for(pixelNum = 0; pixelNum < NUM_LEDS; ) {
        p2  = pgm_read_byte(ptr++); // Data for two pixels...
        p1  = p2 >> 4;              // Shift down 4 bits for first pixel
        p2 &= 0x0F;                 // Mask out low 4 bits for second pixel
        strip.setPixelColor(pixelNum++, palette[p1][0], palette[p1][1], palette[p1][2]);
        strip.setPixelColor((NUM_LEDS_TOTAL - pixelNum), palette[p1][0], palette[p1][1], palette[p1][2]);
        
        strip.setPixelColor(pixelNum++, palette[p2][0], palette[p2][1], palette[p2][2]);
        strip.setPixelColor((NUM_LEDS_TOTAL - pixelNum), palette[p2][0], palette[p2][1], palette[p2][2]);        
      }
      break;
    }

    case PALETTE8: { // 8-bit (256 color) PROGMEM-palette-based image
      uint16_t  o;
      uint8_t   pixelNum,
               *ptr = (uint8_t *)&imagePixels[imageLine * NUM_LEDS];
      for(pixelNum = 0; pixelNum < NUM_LEDS; pixelNum++) {
        o = pgm_read_byte(ptr++) * 3; // Offset into imagePalette
        strip.setPixelColor(pixelNum,
          pgm_read_byte(&imagePalette[o]),
          pgm_read_byte(&imagePalette[o + 1]),
          pgm_read_byte(&imagePalette[o + 2]));
        strip.setPixelColor((NUM_LEDS_TOTAL - pixelNum),
          pgm_read_byte(&imagePalette[o]),
          pgm_read_byte(&imagePalette[o + 1]),
          pgm_read_byte(&imagePalette[o + 2]));          
      }
      break;
    }

    case TRUECOLOR: { // 24-bit ('truecolor') image (no palette)
      uint8_t  pixelNum, r, g, b,
              *ptr = (uint8_t *)&imagePixels[imageLine * NUM_LEDS * 3];
      for(pixelNum = 0; pixelNum < NUM_LEDS; pixelNum++) {
        r = pgm_read_byte(ptr++);
        g = pgm_read_byte(ptr++);
        b = pgm_read_byte(ptr++);
        strip.setPixelColor(pixelNum, r, g, b);
        strip.setPixelColor((NUM_LEDS_TOTAL - pixelNum), r, g, b);
      }
      break;
    }
  }

  
  if(++imageLine >= imageLines) imageLine = 0; // Next scanline, wrap around

  do { 

    if(myDecoder.value != None) {
      if(!strip.getBrightness()) { // If strip is off...
        // Set brightness to last level
        strip.setBrightness(pgm_read_byte(&brightness[bLevel]));
        // and ignore button press (don't fall through)
        // effectively, first press is 'wake'
      } else {
        switch(myDecoder.value) {
         case VolumeUp: // increase brightness 
          Serial.println(">increase brightness");
          if(bLevel < (sizeof(brightness) - 1))
            strip.setBrightness(pgm_read_byte(&brightness[++bLevel]));
          break;
         case VolumeDown: // decrease brightness
          Serial.println(">decrease brightness");
          if(bLevel)
            strip.setBrightness(pgm_read_byte(&brightness[--bLevel]));
          break;
         case UpArrow: // increase speed
          Serial.println(">increase speed");
          if(lineIntervalIndex < (sizeof(lineTable) / sizeof(lineTable[0]) - 1))
            lineInterval = pgm_read_word(&lineTable[++lineIntervalIndex]);
          break;
         case DownArrow: // decrease speed
          Serial.println(">decrease speed");
          if(lineIntervalIndex)
            lineInterval = pgm_read_word(&lineTable[--lineIntervalIndex]);
          break;
         case Back: // restart
          Serial.println(">Restart");
          imageNumber = 0;
          imageInit();
          break;
         case StopMode: // off
          Serial.println(">Off");
          strip.setBrightness(0);
          break;
         case LeftArrow: // previous pattern
          Serial.println(">previous pattern");
          prevImage();
          break;
         case RightArrow: // next pattern
          Serial.println(">next pattern");
          nextImage();
          break;
         case PlayPause: // toggle auto cycle
          Serial.println(">Toggle Autoplay");
	        autoCycle = !autoCycle;
          break;
        }
      }
      myDecoder.value = None;
    }
  }while(((t = micros()) - lastLineTime) < lineInterval);

  strip.show(); // Refresh LEDs
  
  lastLineTime = t;
}


void IRinterrupt() {

  if (myReceiver.getResults()) {
    //Serial.println("results");
    myDecoder.decode();           //Decode it
    //myDecoder.dumpResults(true);
    if(myDecoder.value != 0) {
      
      // myDecoder.dumpResults(true);
      irCode = myDecoder.value;
      
      Serial.println("");
      
      Serial.print("myDecoder.value: ");
      Serial.print(myDecoder.value, HEX);      
      Serial.print("=");
      switch(myDecoder.value) {
          case UpArrow:
            Serial.println("increase speed");
            break;
          case DownArrow:
            Serial.println("decrease speed");
            break;
          case LeftArrow:
            Serial.println("previous pattern");
            break;
          case RightArrow: 
            Serial.println("next pattern"); 
            break;
          case VolumeUp:
            Serial.println("increase brightness");
            break;
          case VolumeDown:
            Serial.println("decrease brightness");
            break;
          case PlayPause:
            Serial.println("Toggle Autoplay");
            break;
          case Back: 
            Serial.println("Restart");
            break;
          case StopMode: 
            Serial.println("Off");
            break;
          case Setup: 
            Serial.println("Battery");
            break;
      }
    } else {
      Serial.print("0");
    }
    myReceiver.enableIRIn();      //Restart receiver
  }
}
