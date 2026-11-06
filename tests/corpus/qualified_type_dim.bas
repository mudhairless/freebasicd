'@fbc:pass
' A dot-qualified type in a type-first declaration is one type: both
' variables below are declared after `dim as gfx.sprite ptr`, and the type's
' leaf `sprite` is never a variable name.
namespace gfx
    type sprite
        w as integer
    end type
end namespace

dim as gfx.sprite ptr firstImage
dim as gfx.sprite ptr secondImage
