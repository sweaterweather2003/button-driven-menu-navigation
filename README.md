# button-driven-menu-navigation
button-driven menu navigation system for a small embedded device with a limited user interface

https://wokwi.com/projects/455627592697353217


*Challenges*:
During development, I faced a couple of challenges:

- Navigation Not Updating LEDs: BTN3 singles didn't set update_display but I solved it by adding (update_display = true) in those cases.
- Submenu Patterns Confusing: Mode Select looked like "reverting to LED 1" – fixed by using distinct bars (LEDs 1-2 for Manual, 3-4 for Auto).
- Reset Not Resetting Selection: Double-press kept old sel – added selected_item = 0 for clean return to Brightness.
- Temp Vars Uninitialized: Could cause garbage – fixed with explicit inits.
- Brightness PWM: Tested inversion for correct dimming (higher value = brighter).

- All these challenges were fixed and the simulation runs smoothly and perfectly and matches the requirements for each button’s use case.
  Testing and Verification

*Working*

- Button Detection: Pressed and released BTN1 quickly → Serial "Queued SINGLE"; double BTN2 → "Queued DOUBLE"; hold BTN3 >1.5s → "Queued LONG".
- Main Menu Navigation: Boot → LED1 lit; BTN1 once → LED2; BTN1 again → LED3; BTN3 once → LED2 back.
- Brightness Submenu: Selected LED1, BTN2 → 5 LEDs lit; BTN2 → 6 LEDs; BTN3 → 4 LEDs; BTN1 → back to Main.
- Mode Select: Selected LED2, BTN2 → LEDs 1-2 lit (Manual); BTN1 → LEDs 3-4 (Auto); BTN3 → cancel back.
- Manual Mode: In Mode (Manual), BTN2 → all LEDs (Pattern1); BTN1 → alternating; BTN2 → save back; BTN3 → cancel.
- Auto Mode: In Mode (Auto), BTN2 → cycles every 2s; BTN1 → exit back.
- Info Submenu: Selected LED3, BTN2 → LEDs 1-4; BTN1 → LEDs 5-8; BTN3 → back.
- Reset Submenu: Selected LED4, BTN2 → alternating; double BTN2 → reset to LED1; BTN3 → cancel.
- Power Off: Hold BTN3 >1.5s → all off; hold BTN1 >1.5s → restart to LED1.
- Brightness Scaling: Brightness, adjust → LEDs dim/brighten (test min/max, ran it from decreasing it from 5 to 0 and have also tested 0 to 10).
- Robustness: Rapid presses → no duplicates, invalid presses → ignored.
