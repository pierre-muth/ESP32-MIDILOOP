This is a small USB Midi looper build around a Xiao ESP32S3 on arduino, a LCD display, a clickable rotary encoder and a main button.

## Summary:

The device can be connected to a synthesizer with USB. The device records midi notes from the synth into its loop. It has a small display for the UI.  
The loop length can be define in bars, such as 8, 16, 24, 32 etc...  
A note can be define with timings for note ON to note OFF, pitch value, velocity and an ID. At the end of the loop all the ON notes should be set back to OFF. When the loop is running, the notes are sent according to the recorded timings, on the usb midi output. When the loop is running and recording, notes are recorded into the loop but not echoed back, they will be played back at the next loop.  
The loop speed can be defined in BPM, beat per minute, such as 120BPM.  
We can also records of 4 MIDI CC channels in the loop, graphically represented as a curve, and they can be muted independently. The CC values are sent to the usb midi out at a 50Hz rate by default, when the loop is running and if there is at least a point on the curve, as the values are interpolated from the curve recorded points. The CC values can be input into the loop from the midi input, but also with a rotary encoder as described later. CC values are between 0 to 127.
A physical button runs or stops the loop.

## Project Hardware description:
- Built around a Xiao ESP32S3 on Arduino, managed by plateformio.
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
Pressing/clicking the encoder knob on the selected button activate it and has different action/effect depending of the type of the button. Usually the effects or value editions are propagated to the data models and updates the UI.

### Button types:  
- If it's a numerical **Value**, clicking the encoder activates it and enters into the edition of the value, a second press exits the edition. In edition, rotating the encoder increments or decrements the value if applicable. The button has a short text describing the value.  
- If it's an **Option** button, clicking the encoder activates it and enters into the edition. In edition mode rotating the encoder cycles through the possible option values. Such as an enumeration "on", "off" or "CH1", "CH2", "CH3", "CH4". A second press exits the edition. The button has a short text describing the options.
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
- Label: Play / Record (or armed)	
- Option: Quantization On / Off
- Value: BPM speed
- Label: loop length in bars

### Logic:
The Run / Stop status is toggled by the physical button on D4. 

Run mode is playing the loop and send accordingly the notes on/off on the usb midi output, there is an animated scrolling vertical line representing the play head. Stopping the loop puts the cursor/play head back at the beginning of the loop, send a note off for all note that was turned on, and put back the mode to Play.  

Clicking the Label Play toggles the from Play to Record and has two different effect depending on the Run / Stop Status. Click action on this label toggles from the modes. In Record or Armed mode, the label is activated. Rotating the knob has no effect, only clicking again will leave the mode to switch back to Play and deactivate the button.  
- If the loop is running, toggling to Record put the system into recording the midi notes received, overdubbing the loop. Notes are not erased by the play head, but if the input note is the same and covers an already recorded note, it prolongs it, basically editing its start (note on) or/and its stop (note off). Pressing again toggles the mode back to Play.
- If the loop is not running (Stop), toggling to Record put the system in Armed mode. In this mode (armed), toggling the mode again will go back to Play and stays in Stop status. However, in the Armed state, the first incoming midi note on will start the loop to Run status and switch mode to Record. Pressing again from Record toggles the mode back to Play.

Quantization option, when turned on, makes the timings of the note on / off sent to the midi output to stick on the closest bars when the loop is playing. This option also affects the UI by representing the notes and the bars differently, as described later. 

The BPM speed will affect the duration of a bar, adjusting this value changes in real time the playback speed of the loop.

The loop length is just displayed (read only). No action on click. This can be edited on the Settings page.

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
This page will allow the edition of 4 different curves representing the midi CC values output on the midi usb when the loop is running. We support 4 CC channels, by default 1 to 4 and by default all muted (no cc midi output). Curves are virtually continuous, but output on the usb midi out at a frequency defined in the Settings (50Hz default)

### Left buttons below the main mode button:
- Label: Run / Stop status
- Option: selected curve number (1 to 4)
- Label: Play / Record (or armed)
- Option Midi IN / local
- Value: BPM speed
- Option: Muted on / off
- Value: CC channel (0 to 127)
- Label: Clear curve
- Label: loop length in bars

### Logic:
The Run / Stop status is toggled by the physical button on D4.

Changing the selected curve will change the highlighted curve and which data curve is modified.

Click action for the Play label toggles from the modes (Play / Record (or armed)). In (Record or Armed mode), the label is activated. Clicking again will leave the mode to switch back to Play and deactivate the button.  
In play mode, there are no data edition.  
If we switch to record mode and the loop is Stopped, we use the same functionality of the looper. It mean as soon as we have data to record (midi cc in or rotary encoder movement), we start the loop but we stays in Record for only one cycle. Once we reach the end of the loop, we switch back to play and continue to loop in play.  
Once in record and Run mode, the rotary encoder is used to input the selected curve values. The rotary encoder increments or decrements the selected curve value under the play head.
If we switch to record while the loop is running and Midi IN is selected, the received midi CC of the corresponding channel overrides the curve under the play head.  
if we switch to record while the loop is running and local is selected, the CC input, represented by right triangle follows the curve until we start to rotate the knob. from here the values of the curve will be overridden. The loop continues in record until we click the encoder to exit the edition and switch back to Play mode.   

If we select Midi IN, then we record the only midi in CC values corresponding to the channel. In local, only the rotary encoder is used to built the curve.

The BPM speed will affect the duration of a bar, adjusting this value changes in real time the playback speed of the loop.

Muted curve will not output any midi CC values for its channel.

The CC channel correspond to the output midi CC channel of this curve.

Clicking on Clear curve will erase the points of this curve.

The loop length is just displayed (read only). No action on click. This can be edited on the Settings page.

### Center chart UI:
Selected curve is 3 pixel thick and full. Other curves are 3 pixel thick and gray (chessboard pattern).

### Right column:
On the furthermost right, small (4 pixel) triangles pointing left. If midi CC IN is on, it shows the last incoming midi CC value on the selected channel. If it's off, it is driven by the rotation of the encoder in Record mode.

## Setting mode
This page is dedicated to the settings management. The UI only have the vertical stack of buttons, so the buttons can take take the full width of the screen. The first top button is still the main mode button. As the settings page is the last one, clicking again on the main mode button loop back to the looper mode/page. 

### buttons below the main mode button:
- Value: loop length in bars
- Value: playback speed in BPM
- Value: CC output rate in Hz

### Logic:
Changing the loop length should not erase any notes or CC from the loop. In case of loop length shrinking, the data outside of the loop time are just ignored.

Changing the playback speed happens in real time, so if the loop is running while we change this parameter, the playback speed change accordingly.

Changing the CC output rate changes the frequency the 4 midi CC channels are output.

## Some software guidelines 
Implement different tasks (freeRTOS).  
Midi task running on core 0 with high priority for timing precision.  Be careful with the TWDT watchdog, disable it or manage priority accordingly.
UI is lower priority and can run on core 1, can try to run up to ~20 fps but might need more time for the drawings. Avoid using the clearDisplay() function in the drawing process as the display will immediately goes white and might causes flickering.   
Debug on UART. Set the speed to 115200 bauds so we can have a decent amount of information.
The code of the UI should be flexible enough to be able to add buttons easily, such as a new button in the Setting page.  
Check compilation errors.  


