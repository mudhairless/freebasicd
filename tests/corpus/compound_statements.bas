' Compound statements: several statements on one line, separated by
' colons. A block that opens and closes on the same line must not
' disturb the indentation of anything after it; the last lines of
' this file sit at the top level, outside every sub. The engine
' tests pin the depths; this file puts the shapes through the corpus
' gate under both presets.

sub markUsed( used() as integer, count as integer )
	for i as integer = 0 to count - 1 : used(i) = 0 : next
end sub

function countUp( limit as integer ) as integer
	dim as integer n = 0
	while n < limit : n = n + 1 : wend
	do : n = n - 1 : loop while n > 0
	return n
end function

sub nested( flags() as integer )
	scope
		for s as integer = 0 to 2
			for i as integer = 0 to 1 : flags(i) = 0 : next
		next
	end scope
end sub

dim as integer a = 1 : dim as integer b = 2
print a + b
