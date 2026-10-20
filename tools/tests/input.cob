set name = input("Name? ")
pop("Hello, " + name + "!")

set age = input_int("Age? ")
set next_year = age + 1
pop("Next year you will be")
pop(next_year)

set raw = input()
pop("raw=[" + raw + "]")

set total = 0
set n = input_int("How many numbers? ")
set i = 0
while i < n:
    set total = total + input_int("  value: ")
    set i = i + 1
pop("total")
pop(total)

set last = input("Anything else? ")
if last:
    pop("got something")
else:
    pop("got nothing (EOF or blank)")
