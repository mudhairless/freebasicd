'@fbc:fail
' A parenthesized initializer with too many elements.
type T
    x as integer
end type
dim q as T = (1, 2)
