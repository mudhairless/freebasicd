'@fbc:pass
' M15 type graph: `Extends` is FreeBASIC's only inheritance form, and a
' member procedure is declared inside the type and defined at module level
' qualified by the type name. fbc 1.10.2 accepts every line below; there is
' deliberately no `type d : base` (rejected, even under -lang qb) and no
' `interface` keyword in the language at all.

type shape_t
    n as integer
end type

type circle_t extends shape_t
    r as double
    declare function area() as double
end type

type filled_t extends circle_t
    color as integer
    declare sub describe()
end type

union rec_t extends shape_t
    x as integer
end union

' Inherited field through two levels, and an inherited member call.
dim c as circle_t
c.n = 1
c.r = 2.0
c.area

' The implementations live at module level, qualified. A definition inside
' the type body is fbc error 17, so there is no other spelling.
function circle_t.area() as double
    return r * r
end function

sub filled_t.describe()
    print n, r, color
end sub

' A derived type may not re-implement an inherited member (fbc error 158), so
' `area` above is reached through `filled_t` rather than overridden there.
dim f as filled_t
f.describe
