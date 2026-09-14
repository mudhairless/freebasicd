'@fbc:pass
type vec
    x as single
    y as single
end type

operator + (a as vec, b as vec) as vec
end operator

dim a as vec
dim b as vec
dim c as vec = a + b