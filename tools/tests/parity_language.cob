set a = 9223372036854775807
pop(a + 1)
pop(0 - a - 2)
pop(7 / 0)
pop("x" - 1)
if "a" == 1:
    pop("eq")
else:
    pop("mixed compare is false")
if "abc" < "abd":
    pop("lt ok")
pop(undefined_var)
set s = "tab\there \"q\" back\\slash"
pop(s)
if "": 
    pop("no")
else:
    pop("empty is falsy")
set i = 0
while i < 3:
    set i = i + 1
    if i == 2:
        pop("two")
pop(-5 * -5)
pop(10 - 2 - 3)
pop(2 + 3 * 4)
pop("n=" + 5 + "/" + "x")
recipe sq(n):
    serve n * n
pop(sq(sq(3)))
sq(4)
