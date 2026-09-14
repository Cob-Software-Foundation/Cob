_MakeCache = False
shuck cobwindow
set i = 0
while i < 20:
    set win = window_open("Stress " + i)
    set rc = window_label(win, "Cycle " + i + " of 20")
    set rc = window_wait(win, 0)
    set rc = window_close(win)
    set i = i + 1
pop("window stress: opened/closed " + i + " windows")
