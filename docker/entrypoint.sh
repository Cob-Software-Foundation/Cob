#!/bin/sh
# Cob container entrypoint.
#
#   docker run -it cob                      the REPL
#   docker run cob hello.cob                run a program (mount it: -v "$PWD:/work")
#   docker run cob hello.cob --no-gc        any cob_interp flags work
#   docker run cob popcorn_comp x.strawberry -o x     use the compiler
#   docker run cob farmer harvest <package>           use the package manager
#   docker run -it cob sh                   a shell inside the image
#
# Anything that is not the name of another tool in the image goes to the
# interpreter, so the common case needs no extra typing.
case "${1:-}" in
    popcorn_comp|farmer|sh|bash)
        exec "$@"
        ;;
esac
exec cob_interp "$@"
