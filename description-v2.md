This is a small Midi looper build around an arduino Xiao ESP32S3.

Summary:
The device can be connected to a synthetizer with USB. The device records midi notes from the synth into a loop. 
The loop lenght can be define in bars, such as 8, 16, 24, 32 etc...
A note can be define as timings for note ON to note OFF plus velocity. At the end of the loop all the ON notes should be set back OFF. When the loop is running, the notes are sent according to the timings on the usb midi output. When the loop is running and recording, notes are recorded into the loop but not echoed back, they will be played at the next loop.
The loop speed can be define in BPM, beat per minute, such as 120BPM.
We can also record MIDI CC along the loop for 4 channels. The CC values are input with the rotary incoder into the loop while in CC recording mode and output accordingly on the USB midi ouput when the loop is running.

Hardware description:
It uses USB midi, the library is tttapa/Control Surface
It has a main button connected on D4
It has an rotary encoder, wire A and B connected to D1 and D2. The knob press button is on D3
It has a debug UART: TX on D6, RX on D5 (only TX wire needed)
It has a built in LED connected to LED_BUILTIN

It has a monochrom LCD display, the library is adafruit/Adafruit SHARP Memory Display. SPI connections are the following : 
SHARP_SCK:D8
SHARP_MOSI:D10
SHARP_SS:D7
The display resolution is W:400 pixels to H:240 pixels, viewing area is approx 58mm x 35mm so rather high pixel density, to be taken in account for IU readability. Drawing can be done with the Adafruit_GFX library.

Modes corresponding to different UI pages:
Looper
Note edit
CC edit
Settings

