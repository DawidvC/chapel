config const n = 4;
config const printMemStats = false;

proc create_literal() {
  var D = {1..n};
}

proc return_literal() {
  return {1..n};
}

proc create_by(D: domain(1)) {
  var D2 = D by 2;
}
proc return_by(D: domain(1)) {
  return D by 2;
}

proc create_slice(D: domain(1)) {
  D[2..n-1];
}
proc return_slice(D: domain(1)) {
  return D[2..n-1];
}

proc create_expand(D: domain(1)) {
  D.expand(1);
}
proc return_expand(D: domain(1)) {
  return D.expand(1);
}

proc create_exterior(D: domain(1)) {
  D.exterior(1);
}
proc return_exterior(D: domain(1)) {
  return D.exterior(1);
}

proc create_interior(D: domain(1)) {
  D.interior(1);
}
proc return_interior(D: domain(1)) {
  return D.interior(1);
}

proc return_translate(D: domain(1)) {
  return D.translate(1);
}
proc create_translate(D: domain(1)) {
  D.translate(1);
}

use Memory;
proc main() {
  const D = return_literal();

  writeln("Calling create_literal():");
  var m1 = memoryUsed();
  {
    create_literal();
  }
  var m2 = memoryUsed();
  writeln("\t", m2-m1, " bytes leaked");
  if printMemStats then printMemTable();

  writeln("Calling return_literal():");
  m1 = memoryUsed();
  {
    var D2 = return_literal();
  }
  m2 = memoryUsed();
  writeln("\t", m2-m1, " bytes leaked");
  if printMemStats then printMemTable();

  writeln("Calling create_by():");
  m1 = memoryUsed();
  {
    create_by(D);
  }
  m2 = memoryUsed();
  writeln("\t", m2-m1, " bytes leaked");
  if printMemStats then printMemTable();

  writeln("Calling return_by():");
  m1 = memoryUsed();
  {
    var D2 = return_by(D);
  }
  m2 = memoryUsed();
  writeln("\t", m2-m1, " bytes leaked");
  if printMemStats then printMemTable();

  writeln("Calling create_slice():");
  m1 = memoryUsed();
  {
    create_slice(D);
  }
  m2 = memoryUsed();
  writeln("\t", m2-m1, " bytes leaked");
  if printMemStats then printMemTable();

  writeln("Calling return_slice():");
  m1 = memoryUsed();
  {
    var D2 = return_slice(D);
  }
  m2 = memoryUsed();
  writeln("\t", m2-m1, " bytes leaked");
  if printMemStats then printMemTable();

  writeln("Calling create_expand():");
  m1 = memoryUsed();
  {
    create_expand(D);
  }
  m2 = memoryUsed();
  writeln("\t", m2-m1, " bytes leaked");
  if printMemStats then printMemTable();

  writeln("Calling return_expand():");
  m1 = memoryUsed();
  {
    var D2 = return_expand(D);
  }
  m2 = memoryUsed();
  writeln("\t", m2-m1, " bytes leaked");
  if printMemStats then printMemTable();

  writeln("Calling create_exterior():");
  m1 = memoryUsed();
  {
    create_exterior(D);
  }
  m2 = memoryUsed();
  writeln("\t", m2-m1, " bytes leaked");
  if printMemStats then printMemTable();

  writeln("Calling return_exterior():");
  m1 = memoryUsed();
  {
    var D2 = return_exterior(D);
  }
  m2 = memoryUsed();
  writeln("\t", m2-m1, " bytes leaked");
  if printMemStats then printMemTable();

  writeln("Calling create_interior():");
  m1 = memoryUsed();
  {
    create_interior(D);
  }
  m2 = memoryUsed();
  writeln("\t", m2-m1, " bytes leaked");
  if printMemStats then printMemTable();

  writeln("Calling return_interior():");
  m1 = memoryUsed();
  {
    var D2 = return_interior(D);
  }
  m2 = memoryUsed();
  writeln("\t", m2-m1, " bytes leaked");
  if printMemStats then printMemTable();

  writeln("Calling create_translate():");
  m1 = memoryUsed();
  {
    create_translate(D);
  }
  m2 = memoryUsed();
  writeln("\t", m2-m1, " bytes leaked");
  if printMemStats then printMemTable();

  writeln("Calling return_translate():");
  m1 = memoryUsed();
  {
    var D2 = return_translate(D);
  }
  m2 = memoryUsed();
  writeln("\t", m2-m1, " bytes leaked");
  if printMemStats then printMemTable();

}


