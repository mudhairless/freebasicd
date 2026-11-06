'@fbc:pass
' Data lists hold signed constants in every slot: a negative literal may
' start the list or follow a comma, and the formatter must keep the sign
' attached to its number in both positions. Sleep and Randomize are the
' single-value shape of the same class (a value directly after the
' keyword); their documented default amount and seed are negative. The
' engine checks pin the spacing; this file keeps the shapes covered end
' to end under both presets.

ship_data:

data 3
data 0, -13
data -5, 13
data -5, 13
data 0, 5

dim as integer v
restore ship_data
read v
sleep -1
randomize -1
