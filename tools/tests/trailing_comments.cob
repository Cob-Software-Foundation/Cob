# A # after code starts a comment; a # inside a string does not (Cob 0.1.0)
set a = 1   # one
set b = "x # y"
pop(a)      # prints 1
pop(b)
pop("tab\t# kept")  # dropped
pop("a \" # b")
set c = 2#no space needed
pop(c)
if a == 1:  # on a block header too
    pop("inside")   # and in the block
recipe f(n):   # and on a recipe line
    serve n + 1  # ok
pop(f(1))
    # an indented comment-only line
pop("done")#end
