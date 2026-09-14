_MakeCache = False
set i = 0
set total = 0
while i < 200000:
    set total = total + i
    set i = i + 1
pop("arithmetic stress: total=" + total + " iterations=" + i)
