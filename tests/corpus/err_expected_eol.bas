'@fbc:fail
' An enum tail after a macro invocation is still end-of-line.
#macro M(x)
    x
#endmacro
enum E
    M(a) 5
end enum
