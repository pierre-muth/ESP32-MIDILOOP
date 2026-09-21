This is a small USB Midi looper build around an arduino Xiao ESP32S3.

### Summary:

The device can be connected to a synthesizer with USB. The device records midi notes from the synth into its loop.  
The loop length can be define in bars, such as 8, 16, 24, 32 etc...  
A note can be define as timings for note ON to note OFF plus velocity. At the end of the loop all the ON notes should be set back to OFF. When the loop is running, the notes are sent according to the recorded timings, on the usb midi output. When the loop is running and recording, notes are recorded into the loop but not echoed back, they will be played back at the next loop.  
The loop speed can be defined in BPM, beat per minute, such as 120BPM.  
We can also record 4 MIDI CC channels in the loop. The CC values are input with the rotary encoder into the loop while in CC edit mode and output accordingly on the USB midi output when the loop is running.  

### Project Hardware description:
- Built around a Xiao ESP32S3.
- It uses USB midi, the library is tttapa/Control Surface
- It has a Run / Stop button connected on D4
- It has an rotary encoder, wire A and B connected to D1 and D2. The knob press button is on D3
- It has a debug UART: TX on D6, RX on D5 (only TX wire needed)
- It has a built in LED connected to LED_BUILTIN

- It has a monochrom LCD display, the library is adafruit/Adafruit SHARP Memory Display. SPI connections are the following :

  - SHARP_SCK:D8
  - SHARP_MOSI:D10
  - SHARP_SS:D7

The display resolution is W:400 pixels to H:240 pixels, viewing area is approx 58mm x 35mm so rather high pixel density, to be taken in account for IU readability. Drawing can be done with the Adafruit_GFX library.

### Common UI navigation:

The rotary encoder rotation cycle through all the UI button. Pressing the encoder knob on the highlighted button has different action depending of the type of the button.  
If it's a numerical Value, it enters into the edition of the value by rotating the encoder, a second press exits the edition.  
If it's an Option button, it enter in edit mode and cycle through the possible option values. Such as "on", "off" or "CH1", "CH2", "CH3", "CH4".  
If it's the mode button, pressing the encoder switch the UI to the next mode/UI page. It displays the name of the mode/page.  
And finally, some buttons are just Label, non editable, can display a number or a string.  

The buttons are stacked vertically on left side of the screen, leaving the center majority of the screen for the chart. The top button is the mode/page switch button.  
The center part UI and behavior is defined for each mode later.

### The 4 Modes corresponding to different UI pages:

- Looper
- Note edit
- CC edit
- Settings

### Looper mode

#### Left buttons below the mode one:
- Label: Run / Stop status
- Option: Play / Record (or armed)
- Option: Quantization On / Off
- Value: BPM speed
- Label: loop length in bars

#### Center chart UI:


#### Logic:
The Run / Stop status is toggled by the physical button on D4. Run mode is playing the loop and send accordingly the notes on/off on the usb midi output. Stopping the loop puts the cursor/play head back at the beginning of the loop and put back the mode to Play.  

Toggling the from Play to record has two different effect depending on the Run / Stop Status.  
- If the loop is running, toggling to Record put the system into recording the midi notes received, overdubbing the loop. Notes are not erased by the play head, but if the input note is the same and covers an already recorded note, it prolongs it, basically editing its start (note on) or/and its stop (note off).
- If the loop is not running (Stop), toggling to Record put the system in Armed mode. In this mode (armed), toggling the mode again will go back to Play and stays in Stop status. However, in the Armed state, the first incoming midi note on will start the loop to Run status and Record mode.

Quantization option, when turned on, makes the timings of the note on / off sent to the midi output to stick on the closest bars when the loop is playing.

The BPM speed will affect the duration of a bar, adjusting this value changes in real time the playback speed of the loop.

The loop length is just displayed (read only). This can be edited on the Settings page.


### Note edition
(todo)
for a new note, the received midi note can be used to position the note value.


### software guideline
(todo) 
different tasks, midi task running on one core high priority for timing precision, UI is lower priority, can run at 20 fps. 




