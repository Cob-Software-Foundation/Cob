set x = 5
if x > 3:
    pop("big")
else:
    pop("small")

set i = 0
while i < 7:
    if i == 1:
        pop("one")
    elif i == 2:
        pop("two")
    elif i < 5:
        pop("lt5")
        if i == 4:
            pop("  four (nested)")
    else:
        pop("big")
    set i = i + 1

set name = "cob"
if name == "cob":
    pop("str eq ok")
if name != "cob":
    pop("SHOULD NOT PRINT")
set z = 0
if z:
    pop("SHOULD NOT PRINT")
else:
    pop("zero is falsy")
set e = ""
if e:
    pop("SHOULD NOT PRINT")
pop("done")
