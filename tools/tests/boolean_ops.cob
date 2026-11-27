# and / or / not in conditions (Cob 0.1.0), and # comments after code
set a = 5
set b = 10
set name = "cob"   # a trailing comment
pop("# not a comment")  # but this is
pop("quote \" then # still text")   # and this is a comment
if a < b and b < 20:
    pop("and: yes")
if a > b or b == 10:
    pop("or: yes")
if not a > b:
    pop("not: yes")
if not (a > b or b > 100) and name == "cob":
    pop("grouped: yes")
if a > b and b < 20:
    pop("WRONG")
else:
    pop("and-else: ok")
if a > 100:
    pop("WRONG")
elif a < 3 or b > 5 and a == 5:
    pop("elif with and/or: ok")
else:
    pop("WRONG")
# precedence: and binds tighter than or
if 1 == 1 or 1 == 2 and 1 == 3:
    pop("precedence: ok")
if (1 == 1 or 1 == 2) and 1 == 3:
    pop("WRONG")
else:
    pop("parens: ok")
# while re-tests the whole condition every pass
set i = 0
set j = 10
while i < 5 and j > 7:
    set i = i + 1
    set j = j - 1
pop(i)
pop(j)
# short-circuit: the second side must not run when the first decides
recipe noisy(x):
    pop("noisy ran")
    serve x
if 0 == 1 and noisy(1) == 1:
    pop("WRONG")
if 1 == 1 or noisy(1) == 1:
    pop("short-circuit: ok")
if 0 == 1 or noisy(1) == 1:
    pop("or evaluates the right side when it must")
# strings and truthiness
set s = ""
if not s:
    pop("empty string is false")
set s = "x"
if s and not 0:
    pop("truthy: ok")
# a variable called not / and / or still works
set not = 7
if not == 7:
    pop("var named not")
set or = 1
if or:
    pop("var named or")
# inside a recipe, nested
recipe classify(n):
    if n < 0 or n > 100:
        serve "out"
    elif n >= 10 and not n == 50:
        serve "big"
    else:
        serve "small"
pop(classify(-1))
pop(classify(10))
pop(classify(50))
pop(classify(3))
