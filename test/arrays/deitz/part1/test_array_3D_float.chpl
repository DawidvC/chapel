config var n : int = 4;
var d = [1..n, 1..n, 1..n];
var a : [d] float;

for (i,j,k) in d do
  a(i,j,k) = 1.0 * ((i-1)*n**2 + (j-1)*n + k);

for ijk in d do
  writeln(a(ijk));
