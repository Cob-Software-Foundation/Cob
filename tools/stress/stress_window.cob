_MakeCache = False
shuck barn
set i = 0
while i < 20:
    set win = barn_open("Stress " + i)
    set rc = barn_label(win, "Cycle " + i + " of 20")
    set rc = barn_wait(win, 0)
    set rc = barn_close(win)
    set i = i + 1
pop("window stress: opened/closed " + i + " windows")
