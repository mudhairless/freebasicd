'@fbc:fail
' A type used before its definition is still an error.
dim x as T
type T
    n as integer
end type
