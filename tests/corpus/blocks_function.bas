'@fbc:pass
function clamp(v as single, lo as single, hi as single) as single
    if v < lo then
        return lo
    elseif v > hi then
        return hi
    end if
    return v
end function

print clamp(5, 1, 10)