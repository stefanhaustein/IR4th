# math

[← Library Index](README.md) · Source: [lib/math.ir4](../../lib/math.ir4)

Portable floating-point math library for IR4th

## Functions

- **arctan** ( x :f -> :f ) — Arctangent of *x* in [-pi/2, pi/2]
- **atan2** ( y :f x :f -> :f ) — Two-argument arctangent atan2(*y*, *x*) in [-pi, pi]
- **cos** ( x :f -> :f ) — Cosine of *x* (radians)
- **exp** ( x :f -> :f ) — Exponential function e^*x*
- **fabs** ( x :f -> :f ) — Absolute value of *x*
- **fint** ( x :f -> :f ) — Truncate *x* toward zero, returning float
- **frac** ( x :f -> :f ) — Fractional part of *x* (*x* - fint(*x*))
- **fround** ( x :f -> :i ) — Round *x* to nearest integer (half away from zero)
- **ln** ( x :f -> :f ) — Natural logarithm ln(*x*) (returns 0.0 for *x* <= 0.0)
- **sin** ( x :f -> :f ) — Sine of *x* (radians)
- **sqrt** ( x :f -> :f ) — Square root of *x* (returns 0.0 for *x* <= 0.0)
