_MakeCache = False
shuck barn
set w = barn_open("Widget Visual Check")
set rc = barn_label(w, "Hello from Cob widgets!")
set rc = barn_button(w, "Click me")
set rc = barn_slider(w, "Volume", 100)
set rc = barn_textbox(w, "Name")
set rc = barn_wait(w, 3)
set rc = barn_close(w)
