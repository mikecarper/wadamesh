// SPDX-License-Identifier: GPL-3.0-or-later
// Same boundary rules as the Keymind Cascade collar, in TAK GPS microdegrees.
#include "BikeRide.h"
#include <math.h>
#include <algorithm>
namespace bike {
constexpr int32_t dateline = 180000000, pole = 90000000;
static int32_t longitude_offset(int32_t lon, int32_t origin) {
  int64_t diff = int64_t(lon) - origin;
  if (diff > dateline) diff -= 2LL * dateline;
  if (diff < -dateline) diff += 2LL * dateline;
  return int32_t(diff);
}

static bool between(int32_t v, int32_t a, int32_t b) {
  return (a <= v && v <= b) || (b <= v && v <= a);
}

// Compare the cross products separately so even world-scale inputs fit in int64.
static int orientation(Point a, Point b, Point p) {
  int64_t left = (int64_t(b.lon) - a.lon) * (p.lat - a.lat);
  int64_t right = (int64_t(p.lon) - a.lon) * (b.lat - a.lat);
  return (left > right) - (left < right);
}

static bool in_bounds(Point a, Point b, Point p) {
  return between(p.lat, a.lat, b.lat) && between(p.lon, a.lon, b.lon);
}

static bool intersects(Point a, Point b, Point c, Point d) {
  int abc = orientation(a, b, c), abd = orientation(a, b, d);
  int cda = orientation(c, d, a), cdb = orientation(c, d, b);
  return (abc * abd < 0 && cda * cdb < 0) ||
      (!abc && in_bounds(a, b, c)) || (!abd && in_bounds(a, b, d)) ||
      (!cda && in_bounds(c, d, a)) || (!cdb && in_bounds(c, d, b));
}

bool valid(const Boundary& shape) {
  unsigned n = shape.count;
  if (!n || n > 9) return false;
  Point v[9];
  int32_t west = 0, east = 0;
  for (unsigned i = 0; i < n; ++i) {
    Point p = shape.points[i];
    if (p.lat < -pole || p.lat > pole || p.lon < -dateline || p.lon > dateline) return false;
    v[i] = {p.lat, longitude_offset(p.lon, shape.points[0].lon)};
    west = std::min(west, v[i].lon);
    east = std::max(east, v[i].lon);
  }
  if (n == 1) return shape.radius > 0;
  if (n == 2) return v[0].lat != v[1].lat && v[1].lon != 0;
  if (int64_t(east) - west >= dateline) return false;
  for (unsigned i = 0; i < n; ++i) {
    Point a = v[i], b = v[(i + 1) % n], c = v[(i + 2) % n];
    if ((a.lat == b.lat && a.lon == b.lon) ||
        (!orientation(a, b, c) && !in_bounds(a, c, b))) return false;
    for (unsigned j = i + 2; j < n; ++j) {
      if ((j + 1) % n != i && intersects(a, b, v[j], v[(j + 1) % n])) return false;
    }
  }
  return true;
}

bool contains(const Boundary& shape, Point p) {
  const Point a = shape.points[0];
  const int32_t x = longitude_offset(p.lon, a.lon);
  if (shape.count == 1) {
    // Compare haversines directly. A metre-scale delta retains its precision
    // because we subtract the integer coordinates before converting to radians.
    constexpr double rad = 1.7453292519943295e-8, earth_radius_m = 6371008.8;
    if (shape.radius >= 20015115) return true;  // Half the Earth's circumference.
    double dy = sin((p.lat - a.lat) * (rad / 2));
    double dx = sin(x * (rad / 2));
    double limit = sin(shape.radius / (2 * earth_radius_m));
    double cos_a = sin((pole - abs(a.lat)) * rad);
    double cos_p = sin((pole - abs(p.lat)) * rad);
    return dy * dy + cos_a * cos_p * dx * dx <= limit * limit;
  }
  if (shape.count == 2) {
    Point b = shape.points[1];
    return between(p.lat, a.lat, b.lat) && between(x, 0, longitude_offset(b.lon, a.lon));
  }
  // Ray crossing in longitude/latitude space. Unwrap around the first vertex
  // so a local fence can straddle the date line. Points on an edge are inside.
  p.lon = x;
  bool inside = false;
  Point prev = {a.lat, 0};
  for (unsigned i = 1; i <= shape.count; ++i) {
    Point next = shape.points[i % shape.count];
    next.lon = longitude_offset(next.lon, a.lon);
    int side = orientation(prev, next, p);
    if (!side && in_bounds(prev, next, p)) return true;
    if ((prev.lat > p.lat) != (next.lat > p.lat) && (side > 0) == (next.lat > prev.lat)) inside = !inside;
    prev = next;
  }
  return inside;
}

}
