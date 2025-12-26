/*
 * This file is part of the BSGS distribution (https://github.com/JeanLucPons/Kangaroo).
 * Copyright (c) 2020 Jean Luc PONS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef SECP256K1H
#define SECP256K1H

#include "Point.h"
#include <string>
#include <vector>

class Secp256K1 {

public:

  Secp256K1();
  ~Secp256K1();
  void  Init();
  Point ComputePublicKey(Int *privKey,bool reduce=true);
  std::vector<Point> ComputePublicKeys(std::vector<Int> &privKeys);
  Point NextKey(Point &key);
  bool  EC(Point &p);

  std::string GetPublicKeyHex(bool compressed, Point &p);
  bool ParsePublicKeyHex(std::string str,Point &p,bool &isCompressed);

  Point Add(Point &p1, Point &p2);
  Point Add2(Point &p1, Point &p2);
  Point AddDirect(Point &p1, Point &p2);
  Point Double(Point &p);
  Point DoubleDirect(Point &p);

  std::vector<Point> AddDirect(std::vector<Point> &p1,std::vector<Point> &p2);

  // GLV Endomorphism support for faster computation
  // For secp256k1: φ(x,y) = (βx, y) where β^3 = 1 mod p
  // φ(P) = λP where λ^3 = 1 mod n
  Point ApplyEndomorphism(Point &p);           // Compute φ(P) = (βx, y)
  void  GLVDecompose(Int *k, Int *k1, Int *k2); // Decompose k = k1 + k2*λ

  Point G;                 // Generator
  Point PhiG;              // φ(G) = Endomorphism of generator
  Int   order;             // Curve order

  // GLV constants
  Int beta;                // β: cube root of unity mod p (x-coordinate multiplier)
  Int lambda;              // λ: eigenvalue satisfying φ(P) = λP mod n

private:

  uint8_t GetByte(std::string &str,int idx);

  Int GetY(Int x, bool isEven);
  Point GTable[256*32];       // Generator table

};

#endif // SECP256K1H
