_MakeCache = False
shuck cobwindow
set w = window_open("Widget Visual Check")
set rc = window_label(w, "Hello from Cob widgets!")
set rc = window_button(w, "Click me")
set rc = window_slider(w, "Volume", 100)
set rc = window_textbox(w, "Name")
set rc = window_wait(w, 3)
set rc = window_close(w)
