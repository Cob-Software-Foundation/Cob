shuck porch

set name = porch_ask("Name? ")
pop("Hello, " + name + "!")

set age = porch_ask_int("Age? ")
set next_year = age + 1
pop("Next year you will be")
pop(next_year)

set raw = porch_ask()
pop("raw=[" + raw + "]")

set total = 0
set n = porch_ask_int("How many numbers? ")
set i = 0
while i < n:
    set total = total + porch_ask_int("  value: ")
    set i = i + 1
pop("total")
pop(total)

set last = porch_ask("Anything else? ")
if last:
    pop("got something")
else:
    pop("got nothing (EOF or blank)")
