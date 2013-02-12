inline proc *(v:real, xyz:3*real) {
  return (v*xyz[1], v*xyz[2], v*xyz[3]);
}

inline proc *(xyz:3*real, v:real) {
  return (xyz[1]*v, xyz[2]*v, xyz[3]*v);
}

inline proc /(xyz:3*real, v:real) {
  return (xyz[1]/v, xyz[2]/v, xyz[3]/v);
}

inline proc -=(ref xyz: 3*real, v:real) {
  xyz[1] -= v;
  xyz[2] -= v;
  xyz[3] -= v;
}

inline proc sumOfProduct(t1:3*real, t2:3*real) {
  return t1[1]*t2[1] + t1[2]*t2[2] + t1[3]*t2[3];
}

inline proc sumOfSquare(xyz:3*real) {
  return xyz[1]**2 + xyz[2]**2 + xyz[3]**2;
}

inline proc sum(xyz:3*real) {
  return xyz[1] + xyz[2] + xyz[3];
}