'@fbc:fail
' Array ellipsis and dynamic-in-union checks.
type T
    a(0 to ...) as integer
end type
union U
    b(any) as integer
end union
dim c() as integer
redim c(0 to ...)
