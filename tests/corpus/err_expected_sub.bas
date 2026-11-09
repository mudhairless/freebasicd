'@fbc:fail
' ThreadCall naming a function instead of a sub.
function foo() as integer
    foo = 1
end function
sub bar()
    threadcall foo()
end sub
