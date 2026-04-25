
- [x] On launch, TeeDSP should "inject" itself in the Windows output chain -- set Windows output to whatever TeeDSP is set to listen to (normally the loopback) and set its output to whatever Windows was sending to
- [x] I can hear what seems to be a limiter "pumping" when the signal goes to hot (it's not the compressor). can we get an indicator for this somewhere? 
- [ ] Missing an app icon.
- [ ] Double check the fills on ALL EQ curves, still seeing some strange black fill
- [ ] Add build timestamp to the context menu

- [x] Deep-dive in how the FL Studio "heat map" style spectrogram works, it's incredibly useful.
- [x] middle-click knob to reset
- [x] make curve fill semi-transparent so it doesn't fully occlude the spectrogram
- [x] remove "drag - db-click resets" label from EQ, it's ugly and redundant
- [x] srate and channel count is always 0 Hz - 0 ch
- [x] When picking Focusrite as output, pitch is incorrect
- [x] output device dropbox doesn't update when device enumeration occurs
- [x] When quitting TeeDSP cleanly, set Windows output to TeeDSP:s output (healing the gap, as it were)

