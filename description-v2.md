This is a small USB Midi looper build around an arduino Xiao ESP32S3.

## Summary:

The device can be connected to a synthesizer with USB. The device records midi notes from the synth into its loop.  
The loop length can be define in bars, such as 8, 16, 24, 32 etc...  
A note can be define with timings for note ON to note OFF, pitch value, velocity and an ID. At the end of the loop all the ON notes should be set back to OFF. When the loop is running, the notes are sent according to the recorded timings, on the usb midi output. When the loop is running and recording, notes are recorded into the loop but not echoed back, they will be played back at the next loop.  
The loop speed can be defined in BPM, beat per minute, such as 120BPM.  
We can also record 4 MIDI CC channels in the loop. The CC values are input with the rotary encoder into the loop while in CC edit mode and output accordingly on the USB midi output when the loop is running.  

## Project Hardware description:
- Built around a Xiao ESP32S3.
- It uses USB midi, the library is tttapa/Control Surface
- It has a Run / Stop button connected on D4
- It has an rotary encoder, wire A and B connected to D1 and D2. The knob press button is on D3
- It has a debug UART: TX on D6, RX on D5 (only TX wire needed)
- It has a built in LED connected to LED_BUILTIN

- It has a pure monochrom LCD display, the library is adafruit/Adafruit SHARP Memory Display. SPI connections are the following :

  - SHARP_SCK:D8
  - SHARP_MOSI:D10
  - SHARP_SS:D7

The display resolution is W:400 pixels to H:240 pixels, viewing area is approx 58mm x 35mm so rather high pixel density, to be taken in account for IU readability. Drawing can be done with the Adafruit_GFX library.

## Common UI navigation:
### Buttons behavior:  
Buttons are rectangles with information or icon drawn into. The rectangle outline thickness when not selected is 1 pixel and 3 pixels when selected. When the button is activated, the drawing colors are inverted.
When the selected button is not in an activate state, the rotary encoder rotation cycle the selection through all the UI buttons, selecting one one after the other, only one button is selected at a time.  
Pressing/clicking the encoder knob on the selected button activate it and has different action/effect depending of the type of the button. Usually the effects are propagated to the data models and update the UI.

### Button types:  
- If it's a numerical **Value**, clicking the encoder activates it and enters into the edition of the value, a second press exits the edition. In edition, rotating the encoder increments or decrements the value.
- If it's an **Option** button, clicking the encoder activates it and enters into the edition. In edition mode rotating the encoder cycles through the possible option values. Such as an enumeration "on", "off" or "CH1", "CH2", "CH3", "CH4".  
- If it's the **main** mode button, pressing the encoder switch the UI to the next mode/UI page. It displays the icon of the mode/page. There is no real activation. 
- And finally, some buttons are just **Label**, non editable, can display a number or a string. An action can be executed if clicked. There is no real activation but it blinks (inverting back and forth the colors) quickly 3 times to signify the action has been performed.  

The buttons are stacked vertically on left side of the screen, leaving the center majority of the screen for the chart. The top button is always the mode/page switch button.  
The center part UI and behavior is defined for each mode later.

### The 4 Modes corresponding to different UI pages:
- Looper (icon: a looping arrow)
- Note edit (icon: a black quarter note)
- CC edit (icon: two letters 'CC')
- Settings (a typical cog icon)
The main button will only show the icon described. 

## Looper mode page
This page is for playing and controlling the loop.
On the left the vertical stack of buttons ~25% of the horizontal width. The first top button stays the mode/page switch button.    
Center is the note chart, roughly 75%.  
At the right a 5 pixel column for the RT midi input view.  

### Left buttons below the main mode button:
- Label: Run / Stop status
- Option: Play / Record (or armed)	
- Option: Quantization On / Off
- Value: BPM speed
- Label: loop length in bars

### Logic:
The Run / Stop status is toggled by the physical button on D4. Run mode is playing the loop and send accordingly the notes on/off on the usb midi output, there is an animated scrolling vertical line representing the play head. Stopping the loop puts the cursor/play head back at the beginning of the loop and put back the mode to Play.  

Toggling the from Play to record has two different effect depending on the Run / Stop Status.  
- If the loop is running, toggling to Record put the system into recording the midi notes received, overdubbing the loop. Notes are not erased by the play head, but if the input note is the same and covers an already recorded note, it prolongs it, basically editing its start (note on) or/and its stop (note off). Pressing again toggles the mode back to Play.
- If the loop is not running (Stop), toggling to Record put the system in Armed mode. In this mode (armed), toggling the mode again will go back to Play and stays in Stop status. However, in the Armed state, the first incoming midi note on will start the loop to Run status and switch mode to Record. Pressing again from Record toggles the mode back to Play.

Quantization option, when turned on, makes the timings of the note on / off sent to the midi output to stick on the closest bars when the loop is playing. This option also affects the UI by representing the notes and the bars differently, as described later. 

The BPM speed will affect the duration of a bar, adjusting this value changes in real time the playback speed of the loop.

The loop length is just displayed (read only). This can be edited on the Settings page.

### Center chart UI:
The center part takes roughly 75% of the screen width. Its width represents the full loop regardless of the length, and there is a vertical line, animated when the loop is playing, symbolizing the play head. Play direction is left to right.  

Notes are represented by filled horizontal rectangles. Timing of note on is the horizontal start of the rectangle and the end is note off. The rectangle height is 4 pixel, and is placed vertically according to the note pitch. With 4 pixel height the vertical span should be 5 octaves or 60 note values over the 240 screen pixel height.  

If quantization option is on:  
Light dashed vertical line represents the bars (pixel line pattern 1, 0, 0) in the background. The rectangles of the notes are not filled outside of the actual trigger bars if the quantized time value is shortened (later for note on and earlier for note off), and if extended (earlier for note on and later for note off) represented by a 2 pixel line up to the bar.  
If quantization option is off:
Light dashed vertical line only every 8 bars (pixel line pattern 1, 0, 0, 0, 0) in the background. Notes are represented by regular filled horizontal rectangles.

The background shows a grayed version (using a chessboard pattern as the display is monochrome) of the 4 CC curves described in the CC edition part. Only the enabled channels are displayed. 

### Right column:
On the furthermost right, small (4 pixel) triangles pointing left, showing the incoming midi notes, they appear with note on and disappear with note off. 

## Note edition mode
This page will allow the edition of each note recorded in the loop, delete a note and add new notes.  

On the left the vertical stack of buttons ~25% of the horizontal width. The first top button stays the mode/page switch button. They are different of the looper page.
Center is the note chart representing all the notes of the loop, roughly 75%, similar to the looper page.  
At the right a 5 pixel column for the RT midi input view, same as the looper page.  

### Left buttons below the main mode button:
- Value: ID 
- Option: Note name (pitch)
- Value: start
- Value: duration
- Value: velocity
- Option: Quantization On / Off
- Label: New
- Label: Delete

### Logic:
Surround the note rectangle in the chart that is selected by the ID button value, by a rounded rectangle of 3 pixels line width with chessboard pattern (gray).  
Changing the selected ID will cycle through all the recorded notes of the loop. And update the pitch, timing and velocity values.  
Changing the note name or pitch will move the rectangle to the corresponding vertical position.  
Changing the start value will move the start of the note rectangle.
Changing the duration value will move the end of the note rectangle.  
Changing the velocity will only update the velocity value.
Quantization on: changing the start and duration values is only possible in full steps of bars, the center chart vertical bar graduations are every bars.   
Quantization off: changing the start and duration values is possible in tenth of bars. The center chart vertical bar graduations are displayed only every 8 bars.  
Clicking on New will insert a new note into the loop with an incremented ID and select it. The new note pitch is the value of the last received midi note on. The start value is the same bar as the last selected note. Its default duration is one bar.  
Clicking on delete erases the selected note from the loop, and the selection goes to the previous note ID.

### Center chart UI:
Same as the looper page but without the CC curves on the background. The vertical play head line is not drawn. Light dashed vertical line represents the bars (pixel line pattern 1, 0, 0). can be every bars if quantization is on or every 8 bars if off.
The selected note rectangle (by its ID) is surrounded by a rounded rectangle of 3 pixels line width with chessboard pattern (gray).  

### Right column:
On the furthermost right, small (4 pixel) triangles pointing left, showing the incoming midi notes, they appear with note on and disappear with note off. 

## CC edition mode
This page will allow the edition of 4 different curves representing the midi CC values output on the midi usb. We support 4 CC channels, by default 1 to 4 and all muted (no cc midi output).
(todo) 256 dots per loop?

## Setting mode

(todo)

## Some software guidelines
(todo) 
different tasks, midi task running on one core high priority for timing precision, UI is lower priority, can run at 20 fps. Put here useful details of the previous version debug. 



