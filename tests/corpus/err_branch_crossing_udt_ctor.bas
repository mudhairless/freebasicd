'@fbc:fail
type T
    x as integer
    declare constructor()
end type

sub f()
    goto L
    dim u as T
    L:
end sub
